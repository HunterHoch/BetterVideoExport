// VYNE BakeFX: Rockstar Editor bake -> ReShade render-effects bridge
// Based on ReShade's BSD-3-Clause/MIT "effects_during_frame" example.
// This add-on NEVER changes normal gameplay. It only operates with a live
// BetterVideoExport ASI mapping after the first actual video sample.
#include <Windows.h>
#include <d3d11.h>
#include <reshade.hpp>
#include "state_tracking.hpp"
#include "../src/NativeBakeBridge.h"
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <string>

using namespace reshade::api;

namespace {
    HMODULE g_self = nullptr;
    HANDLE g_fileMap = nullptr;
    nativebake::Shared* g_shared = nullptr;
    std::mutex g_candidates;
    std::atomic<effect_runtime*> g_runtime{nullptr};
    thread_local bool g_inEffects = false;

    struct __declspec(uuid("ea908491-c2da-4d0f-b80d-d14985bf1a9f")) CommandState {
        resource_view currentRTV{0};
        bool multiple = false;
    };

    void Connect() {
        if (g_shared) return;
        HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, nativebake::MappingName);
        if (!h) return;
        auto* p = static_cast<nativebake::Shared*>(
            MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(nativebake::Shared)));
        if (!p || p->magic != nativebake::Magic || p->version != nativebake::Version) {
            if (p) UnmapViewOfFile(p);
            CloseHandle(h);
            return;
        }
        g_fileMap = h;
        g_shared = p;
        InterlockedExchange(&p->reshadeAttached, 1);
        reshade::log::message(reshade::log::level::info,
            "[VYNE_BakeFX] Connected to BetterVideoExport shared state");
    }

    // Optional named-resource recognition, only available when the game sets
    // an actual D3D11 debug-object name (Rockstar's internal name is NOT
    // guaranteed to be exposed as a DX11 debug name).
    void GetDebugName(device* d, resource r, char* buffer, size_t capacity) {
        buffer[0] = '\0';
        if (d->get_api() != device_api::d3d11 || r.handle < 0x10000) return;
        static const GUID DebugName = {0x429b8c22,0x9188,0x4b0c,
            {0x87,0x42,0xac,0xb0,0xbf,0x85,0xc2,0x00}};
        auto* native = reinterpret_cast<ID3D11Resource*>(r.handle);
        UINT size = static_cast<UINT>(capacity - 1);
        // For D3D11, ReShade api::resource is the underlying ID3D11Resource.
        if (SUCCEEDED(native->GetPrivateData(DebugName, &size, buffer))) {
            buffer[std::min<size_t>(size, capacity - 1)] = '\0';
        }
    }

    // Returns stable 1-based candidate index or zero if capacity is exhausted.
    LONG RegisterCandidate(nativebake::Shared* p, device* d,
                           resource r, const resource_desc& desc) {
        std::lock_guard<std::mutex> lock(g_candidates);
        const LONG count = std::min<LONG>(static_cast<LONG>(p->candidateCount), static_cast<LONG>(nativebake::MaxCandidates));
        for (LONG i = 0; i < count; ++i) {
            if (p->candidates[i].resource == r.handle) {
                InterlockedIncrement(&p->candidates[i].bindCount);
                return i + 1;
            }
        }
        if (count >= nativebake::MaxCandidates) return 0;
        auto& c = p->candidates[count];
        c.resource = r.handle;
        c.width = desc.texture.width;
        c.height = desc.texture.height;
        c.format = static_cast<LONG>(desc.texture.format);
        c.bindCount = 1;
        c.appliedCount = 0;
        GetDebugName(d, r, c.debugName, sizeof(c.debugName));
        InterlockedExchange(&p->candidateCount, count + 1);
        if (std::strstr(c.debugName, "VideoEncode") ||
            std::strstr(c.debugName, "videoencode")) {
            InterlockedExchange(&p->autoTarget, count + 1);
        }
        char log[256];
        std::snprintf(log, sizeof(log),
            "[VYNE_BakeFX] Candidate #%d: GPU=0x%llX, %ux%u, format=%u, name=%s",
            count + 1, static_cast<unsigned long long>(r.handle),
            desc.texture.width, desc.texture.height,
            static_cast<unsigned>(desc.texture.format),
            c.debugName[0] ? c.debugName : "<unnamed>");
        reshade::log::message(reshade::log::level::info, log);
        return count + 1;
    }


    // Capture one GPU snapshot of every 2560x1440 8-bit BGRA candidate
    // after real export samples have begun. We need visual proof of which
    // target contains the actual scene BEFORE enabling render_effects.
    // This does not modify the GPU resource, game image or encoder input.
    bool WriteBmp(const wchar_t* path, const std::uint8_t* data,
                  unsigned width, unsigned height, unsigned stride) {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        BITMAPFILEHEADER fileHeader{};
        BITMAPINFOHEADER imageHeader{};
        fileHeader.bfType = 0x4D42; // BM
        fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(imageHeader);
        fileHeader.bfSize = fileHeader.bfOffBits + width * height * 4;
        imageHeader.biSize = sizeof(imageHeader);
        imageHeader.biWidth = static_cast<LONG>(width);
        imageHeader.biHeight = -static_cast<LONG>(height); // top-down
        imageHeader.biPlanes = 1;
        imageHeader.biBitCount = 32;
        imageHeader.biCompression = BI_RGB;
        DWORD wrote = 0;
        bool okay = WriteFile(file, &fileHeader, sizeof(fileHeader), &wrote, nullptr) &&
                    wrote == sizeof(fileHeader);
        okay = okay && WriteFile(file, &imageHeader, sizeof(imageHeader), &wrote, nullptr) &&
               wrote == sizeof(imageHeader);
        for (unsigned row = 0; row < height && okay; ++row) {
            okay = WriteFile(file, data + static_cast<size_t>(row) * stride,
                             width * 4, &wrote, nullptr) && wrote == width * 4;
        }
        CloseHandle(file);
        if (!okay) DeleteFileW(path);
        return okay;
    }

    bool SaveGpuSnapshot(command_list* cmd, resource gpuResource,
                         LONG candidate, LONG sample) {
        if (gpuResource.handle < 0x10000 || cmd->get_device()->get_api() != device_api::d3d11)
            return false;
        auto* srcResource = reinterpret_cast<ID3D11Resource*>(gpuResource.handle);
        ID3D11Texture2D* src = nullptr;
        if (FAILED(srcResource->QueryInterface(__uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&src))) || !src)
            return false;
        D3D11_TEXTURE2D_DESC desc{};
        src->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            desc.SampleDesc.Count != 1 || desc.ArraySize != 1 ||
            desc.MipLevels != 1) {
            src->Release();
            return false;
        }
        auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(cmd->get_native());
        if (!ctx) { src->Release(); return false; }
        ID3D11Device* device = nullptr;
        src->GetDevice(&device);
        if (!device) { src->Release(); return false; }
        D3D11_TEXTURE2D_DESC stageDesc = desc;
        stageDesc.Usage = D3D11_USAGE_STAGING;
        stageDesc.BindFlags = 0;
        stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stageDesc.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        const HRESULT createResult = device->CreateTexture2D(&stageDesc, nullptr, &staging);
        bool saved = false;
        if (SUCCEEDED(createResult) && staging) {
            ctx->CopyResource(staging, src);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                wchar_t localAppData[MAX_PATH]{};
                const DWORD count = GetEnvironmentVariableW(
                    L"LOCALAPPDATA", localAppData, MAX_PATH);
                std::wstring path;
                if (count && count < MAX_PATH)
                    path = std::wstring(localAppData) +
                           L"\\FiveM\\FiveM.app\\plugins\\";
                else {
                    wchar_t temp[MAX_PATH]{};
                    GetTempPathW(MAX_PATH, temp);
                    path = temp;
                }
                path += L"VYNE_BakeFX_candidate" + std::to_wstring(candidate) +
                        L"_sample" + std::to_wstring(sample) + L".bmp";
                saved = WriteBmp(path.c_str(),
                                 static_cast<const std::uint8_t*>(mapped.pData),
                                 desc.Width, desc.Height, mapped.RowPitch);
                ctx->Unmap(staging, 0);
                if (saved) {
                    char message[256];
                    std::snprintf(message, sizeof(message),
                        "[VYNE_BakeFX] Saved GPU candidate %ld at native sample %ld, %ux%u, as BMP in FiveM.app/plugins",
                        candidate, sample, desc.Width, desc.Height);
                    reshade::log::message(reshade::log::level::info, message);
                }
            }
        }
        if (staging) staging->Release();
        device->Release();
        src->Release();
        return saved;
    }

    // Called at the end of a render-target pass, before the app binds the
    // next RTV. The output-size check alone is diagnostic, NOT authorization
    // to draw effects. Selection requires an explicit candidate or a named
    // VideoEncode resource; unmatched targets stay untouched.
    void FinishPass(command_list* cmd, resource_view oldView, bool multiple) {
        if (g_inEffects || multiple || oldView == 0 || !g_shared) return;
        auto* p = g_shared;
        if (p->magic != nativebake::Magic || !p->active || !p->firstSampleSeen)
            return;
        auto* runtime = g_runtime.load(std::memory_order_acquire);
        if (!runtime || runtime->get_device() != cmd->get_device()) return;
        device* const d = cmd->get_device();
        if (d->get_api() != device_api::d3d11) return;

        const resource r = d->get_resource_from_view(oldView);
        if (r == 0) return;
        if (r == runtime->get_current_back_buffer()) {
            InterlockedIncrement(&p->skippedBackbuffer);
            return; // No user interface/menu contamination
        }
        const resource_desc desc = d->get_resource_desc(r);
        if (desc.type != resource_type::texture_2d ||
            desc.texture.width != static_cast<unsigned>(p->width) ||
            desc.texture.height != static_cast<unsigned>(p->height) ||
            desc.texture.samples != 1)
            return;
        InterlockedIncrement(&p->eligibleBinds);
        const LONG candidate = RegisterCandidate(p, d, r, desc);
        if (!candidate) return;

        // Only take one snapshot per BGRA candidate and only after ten
        // genuine video samples. The snapshot is READ-ONLY and does not
        // modify the ongoing export, unlike our old EVE frame substitution.
        if (p->captureSnapshots && p->submittedSamples >= 10 &&
            desc.texture.format == format::b8g8r8a8_unorm) {
            auto& entry = p->candidates[candidate - 1];
            if (InterlockedCompareExchange(&entry.snapshotStatus, 2, 0) == 0) {
                const bool saved = SaveGpuSnapshot(cmd, r, candidate, p->submittedSamples);
                InterlockedExchange(&entry.snapshotStatus, saved ? 1 : -1);
                if (!saved)
                    reshade::log::message(reshade::log::level::warning,
                        "[VYNE_BakeFX] GPU snapshot failed; original export unaffected");
            }
        }
        if (!p->applyEffects) return;

        const LONG wanted = p->targetCandidate ? p->targetCandidate : p->autoTarget;
        if (candidate != wanted) return;

        // At most one effects pass between two successive Rockstar
        // Media Foundation video samples. This adds no world scene render.
        const LONG sample = p->submittedSamples;
        if (sample <= 0 ||
            InterlockedCompareExchange(&p->lastAppliedSample, sample, sample) == sample)
            return;

        // Set before rendering to prevent recursive event re-entry.
        if (InterlockedExchange(&p->lastAppliedSample, sample) == sample)
            return;
        g_inEffects = true;
        runtime->render_effects(cmd, oldView, oldView);
        if (const auto* previousState = cmd->get_private_data<state_tracking>())
            previousState->apply(cmd);
        g_inEffects = false;
        InterlockedIncrement(&p->effectsApplied);
        InterlockedIncrement(&p->candidates[candidate - 1].appliedCount);
    }

    void InitRuntime(effect_runtime* r) {
        if (r->get_device()->get_api() != device_api::d3d11) return;
        g_runtime.store(r);
        Connect();
        reshade::log::message(reshade::log::level::info,
            "[VYNE_BakeFX] DX11 effect runtime detected");
    }
    void DestroyRuntime(effect_runtime* r) {
        if (g_runtime.load() == r) g_runtime.store(nullptr);
    }
    void InitCommand(command_list* c) { c->create_private_data<CommandState>(); }
    void DestroyCommand(command_list* c) { c->destroy_private_data<CommandState>(); }

    void BindRT(command_list* c, uint32_t count, const resource_view* rtvs,
                resource_view) {
        auto* state = c->get_private_data<CommandState>();
        if (!state || g_inEffects) return;
        const resource_view next = count && rtvs ? rtvs[0] : resource_view{0};
        if (next != state->currentRTV) {
            FinishPass(c, state->currentRTV, state->multiple);
        }
        state->currentRTV = next;
        state->multiple = count > 1;
    }

    void Present(effect_runtime* runtime) {
        if (runtime != g_runtime.load()) return;
        if (!g_shared) Connect();
        if (g_shared && g_shared->active)
            InterlockedIncrement(&g_shared->presentCount);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        if (!reshade::register_addon(module)) return FALSE;
        state_tracking::register_events(); // state must be restored after render_effects
        reshade::register_event<reshade::addon_event::init_effect_runtime>(InitRuntime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(DestroyRuntime);
        reshade::register_event<reshade::addon_event::init_command_list>(InitCommand);
        reshade::register_event<reshade::addon_event::destroy_command_list>(DestroyCommand);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(BindRT);
        reshade::register_event<reshade::addon_event::reshade_present>(Present);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        reshade::unregister_addon(module);
        state_tracking::unregister_events();
        if (g_shared) { UnmapViewOfFile(g_shared); g_shared = nullptr; }
        if (g_fileMap) { CloseHandle(g_fileMap); g_fileMap = nullptr; }
    }
    return TRUE;
}
