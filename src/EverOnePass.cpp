#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "EverOnePass.h"
#include "Config.h"
#include "Logging.h"
#include <Hooking.h>
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {
    using OMSetRT = void (STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
        ID3D11DepthStencilView*);
    OMSetRT s_original = nullptr;
    std::atomic<bool> s_installed{false};
    std::atomic<bool> s_active{false};
    std::atomic<unsigned> s_width{0}, s_height{0}, s_samples{0};
    std::mutex s_lock;
    thread_local bool s_inHook = false;
    ID3D11Texture2D* s_bound = nullptr; // Owns one reference
    struct Target {
        ID3D11Texture2D* texture = nullptr; // Owns one reference
        unsigned index = 0, binds = 0, width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        std::string name;
    };
    std::array<Target, 24> s_targets{};
    unsigned s_targetCount = 0;
    unsigned s_selected = 0;
    unsigned s_copies = 0, s_encodes = 0, s_fallbacks = 0;
    unsigned s_lastSequence = 0, s_sequence = 0;
    unsigned s_frameStamp = 0;
    bool s_frameReady = false;
    std::vector<std::uint8_t> s_bgra;
    std::wstring s_videoFile;

    bool SwapVtable(void** slot, void* replacement, void** original) {
        if (!slot || !*slot) return false;
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
            return false;
        *original = InterlockedExchangePointer(
            reinterpret_cast<void* volatile*>(slot), replacement);
        DWORD unused = 0;
        VirtualProtect(slot, sizeof(void*), old, &unused);
        return *original != nullptr;
    }

    void ClearTargets() {
        for (unsigned i = 0; i < s_targetCount; ++i) {
            if (s_targets[i].texture) s_targets[i].texture->Release();
            s_targets[i] = {};
        }
        s_targetCount = 0;
        if (s_bound) { s_bound->Release(); s_bound = nullptr; }
    }

    bool IsColorFormat(DXGI_FORMAT f) {
        return f == DXGI_FORMAT_B8G8R8A8_UNORM ||
               f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
               f == DXGI_FORMAT_R8G8B8A8_UNORM ||
               f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    }

    std::string DebugName(ID3D11Texture2D* texture) {
        // Standard D3D11 debug object name. Rockstar's internal texture name
        // is NOT necessarily exposed in the D3D11 debug layer.
        static const GUID debugName = { 0x429b8c22, 0x9188, 0x4b0c,
            {0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00} };
        char buf[96]{};
        UINT len = sizeof(buf)-1;
        if (SUCCEEDED(texture->GetPrivateData(debugName, &len, buf))) {
            buf[std::min<size_t>(len, sizeof(buf)-1)] = '\0';
            return buf;
        }
        return {};
    }

    unsigned RegisterTarget(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& desc) {
        // Call under s_lock. Candidate indices are consistent for this export,
        // NOT necessarily stable across independent FiveM launches.
        for (unsigned i = 0; i < s_targetCount; ++i) {
            if (s_targets[i].texture == tex) {
                ++s_targets[i].binds;
                return i+1;
            }
        }
        if (s_targetCount == s_targets.size()) return 0;
        auto& target = s_targets[s_targetCount++];
        tex->AddRef();
        target.texture = tex;
        target.index = s_targetCount;
        target.binds = 1;
        target.width = desc.Width;
        target.height = desc.Height;
        target.format = desc.Format;
        target.name = DebugName(tex);
        logging::InfoF("[EVER1] GPU candidate {}: {}x{}, format={}, name={}",
            target.index, target.width, target.height,
            static_cast<unsigned>(target.format),
            target.name.empty() ? "<unnamed>" : target.name.c_str());
        return target.index;
    }

    unsigned SelectTarget() {
        // Called under s_lock. We only select AFTER the stream has begun.
        // Prefer a positively named VideoEncode texture, then the final
        // persistent 8-bit BGRA render target observed during this bake.
        const unsigned forced = Config::GetUInt(L"Capture", L"EverTarget", 0);
        if (forced) {
            if (forced <= s_targetCount &&
                IsColorFormat(s_targets[forced-1].format)) return forced;
            return 0;
        }
        for (unsigned i = 0; i < s_targetCount; ++i) {
            if (s_targets[i].name.find("VideoEncode") != std::string::npos)
                return i+1;
        }
        if (s_samples.load() < 8) return 0;
        unsigned best = 0;
        for (unsigned i = 0; i < s_targetCount; ++i) {
            const auto& t = s_targets[i];
            if (IsColorFormat(t.format) && t.binds >= 4)
                best = i+1; // Last stable color target is the heuristic.
        }
        return best;
    }

    bool CopyFromGPU(ID3D11DeviceContext* ctx, ID3D11Texture2D* texture,
                     const D3D11_TEXTURE2D_DESC& src) {
        if (!ctx || !texture) return false;
        ID3D11Device* device = nullptr;
        texture->GetDevice(&device);
        if (!device) return false;
        D3D11_TEXTURE2D_DESC d = src;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        ID3D11Texture2D* stage = nullptr;
        bool okay = false;
        if (SUCCEEDED(device->CreateTexture2D(&d, nullptr, &stage)) && stage) {
            // The previous render target was UNBOUND before we got here,
            // so the copy is of the completed pass, not an in-progress draw.
            ctx->CopyResource(stage, texture);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mapped))) {
                const size_t stride = static_cast<size_t>(d.Width) * 4;
                std::vector<std::uint8_t> frame(stride * d.Height);
                const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                                  d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                for (unsigned y = 0; y < d.Height; ++y) {
                    const auto* srcRow = static_cast<const std::uint8_t*>(mapped.pData) +
                                         static_cast<size_t>(y) * mapped.RowPitch;
                    auto* dstRow = frame.data() + static_cast<size_t>(y) * stride;
                    if (!rgba) {
                        std::memcpy(dstRow, srcRow, stride);
                    } else {
                        for (unsigned x = 0; x < d.Width; ++x) {
                            dstRow[4*x+0] = srcRow[4*x+2];
                            dstRow[4*x+1] = srcRow[4*x+1];
                            dstRow[4*x+2] = srcRow[4*x+0];
                            dstRow[4*x+3] = srcRow[4*x+3];
                        }
                    }
                }
                ctx->Unmap(stage, 0);
                {
                    std::lock_guard<std::mutex> guard(s_lock);
                    if (s_active.load()) {
                        s_bgra.swap(frame);
                        s_frameStamp = s_samples.load() + 1;
                        ++s_sequence;
                        ++s_copies;
                        s_frameReady = true;
                        okay = true;
                    }
                }
            }
        }
        if (stage) stage->Release();
        device->Release();
        return okay;
    }

    void AfterUnbind(ID3D11DeviceContext* ctx, ID3D11Texture2D* last) {
        if (!last || !s_active.load()) return;
        D3D11_TEXTURE2D_DESC desc{};
        last->GetDesc(&desc);
        if (desc.Width != s_width || desc.Height != s_height ||
            !IsColorFormat(desc.Format) || desc.SampleDesc.Count != 1 ||
            desc.ArraySize != 1 || desc.MipLevels != 1) return;
        unsigned index = 0, selected = 0;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            if (!s_active.load()) return;
            index = RegisterTarget(last, desc);
            const unsigned target = SelectTarget();
            if (target != s_selected) {
                s_selected = target;
                if (target)
                    logging::InfoF("[EVER1] Selected GPU candidate {} at native video sample {} (0=automatic mode)",
                        target, s_samples.load());
            }
            selected = s_selected;
        }
        if (index && index == selected && s_samples.load() >= 8)
            CopyFromGPU(ctx, last, desc);
    }

    void STDMETHODCALLTYPE HookOM(ID3D11DeviceContext* ctx, UINT views,
                                  ID3D11RenderTargetView* const* rtvs,
                                  ID3D11DepthStencilView* depth) {
        if (!s_original) return;
        if (s_inHook || !s_active.load()) {
            s_original(ctx, views, rtvs, depth);
            return;
        }
        s_inHook = true;
        ID3D11Texture2D* next = nullptr;
        if (views && rtvs && rtvs[0]) {
            ID3D11Resource* resource = nullptr;
            rtvs[0]->GetResource(&resource);
            if (resource) {
                resource->QueryInterface(__uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&next));
                resource->Release();
            }
        }
        ID3D11Texture2D* last = nullptr;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            last = s_bound;
            s_bound = next; // next already has a COM reference
            if (last && next == last) {
                last->Release();
                last = nullptr; // No completed pass: same RT rebound
            }
        }
        // Complete the bind BEFORE copying the previous render target.
        s_original(ctx, views, rtvs, depth);
        if (last) {
            AfterUnbind(ctx, last);
            last->Release();
        }
        s_inHook = false;
    }

    static std::uint8_t Clip(int v) {
        return static_cast<std::uint8_t>(std::max(0, std::min(255, v)));
    }
    void ConvertNV12(const std::vector<std::uint8_t>& bgra,
                     unsigned w, unsigned h, std::vector<std::uint8_t>& out) {
        out.resize(static_cast<size_t>(w)*h*3/2);
        auto* Y = out.data();
        auto* UV = out.data() + static_cast<size_t>(w)*h;
        for (unsigned row=0; row<h; row+=2) {
            for (unsigned col=0; col<w; col+=2) {
                int r=0,g=0,b=0;
                for(unsigned dy=0;dy<2;++dy)
                for(unsigned dx=0;dx<2;++dx) {
                    const size_t p=(static_cast<size_t>(row+dy)*w+col+dx)*4;
                    int pb=bgra[p],pg=bgra[p+1],pr=bgra[p+2];
                    Y[static_cast<size_t>(row+dy)*w+col+dx] =
                        Clip(16+((47*pr+157*pg+16*pb+128)>>8));
                    r+=pr;g+=pg;b+=pb;
                }
                r=(r+2)>>2;g=(g+2)>>2;b=(b+2)>>2;
                const size_t q=static_cast<size_t>(row/2)*w+col;
                UV[q]=Clip(128+((-26*r-87*g+113*b+128)>>8));
                UV[q+1]=Clip(128+((113*r-103*g-10*b+128)>>8));
            }
        }
    }

    bool Install() {
        // The FiveM-supported vtable probe is the same mechanism as our
        // original EVE experiment. Crucially, this NEVER calls Present().
        HWND window=CreateWindowExW(0,L"STATIC",L"VYNE_EVER_ONEPASS",WS_POPUP,
            0,0,16,16,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if (!window) return false;
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount=1;
        desc.BufferDesc.Width=16;
        desc.BufferDesc.Height=16;
        desc.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow=window;
        desc.SampleDesc.Count=1;
        desc.Windowed=TRUE;
        desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        IDXGISwapChain* chain=nullptr;
        ID3D11Device* device=nullptr;
        ID3D11DeviceContext* context=nullptr;
        D3D_FEATURE_LEVEL level{};
        const HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,
            nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,
            &chain,&device,&level,&context);
        if (FAILED(hr) || !context) {
            logging::WarningF("[EVER1] D3D11 probe unavailable HRESULT=0x{:08X}",
                              static_cast<unsigned>(hr));
            if(context)context->Release();
            if(device)device->Release();
            if(chain)chain->Release();
            DestroyWindow(window);
            return false;
        }
        auto** vtable=*reinterpret_cast<void***>(context);
        void* original=nullptr;
        const bool okay=SwapVtable(vtable+33,
            reinterpret_cast<void*>(&HookOM),&original);
        s_original=reinterpret_cast<OMSetRT>(original);
        context->Release();
        device->Release();
        chain->Release();
        DestroyWindow(window);
        if(okay)logging::Info("[EVER1] DX11 OMSetRenderTargets hook installed; direct offscreen capture, NO Present(TEST)");
        return okay;
    }
}
namespace EverOnePass {
    void Initialize() {
        if (s_installed.exchange(true)) return;
        if (Config::GetUInt(L"Capture",L"EverOnePass",0) == 0) {
            logging::Info("[EVER1] Capture disabled");
            return;
        }
        if (!Install())
            logging::Warning("[EVER1] Capture hook failed; original NV12 encoding remains available");
    }

    void Begin(unsigned w,unsigned h,const std::wstring& filename) {
        if (Config::GetUInt(L"Capture",L"EverOnePass",0)==0 || !s_original) return;
        if(!w || !h || (w&1) || (h&1) || w>3840 || h>2160) {
            logging::Warning("[EVER1] Unsupported dimensions; stock NV12 input retained");
            return;
        }
        std::lock_guard<std::mutex> guard(s_lock);
        s_active=false;
        ClearTargets();
        s_width=w; s_height=h; s_samples=0;
        s_selected=0; s_lastSequence=0; s_sequence=0;
        s_copies=0; s_encodes=0; s_fallbacks=0;
        s_frameReady=false; s_frameStamp=0;
        s_bgra.clear(); s_videoFile=filename;
        s_active=true;
        logging::InfoF("[EVER1] Native one-pass capture armed at {}x{}; no temporal sampling; fallback to GTA NV12 until GPU source is identified",
            w,h);
    }
    void OnVideoSample() {
        if (s_active.load()) ++s_samples;
    }
    bool Consume(std::vector<std::uint8_t>& nv12) {
        if(!s_active.load())return false;
        std::vector<std::uint8_t> frame;
        unsigned w=s_width.load(),h=s_height.load();
        {
            std::lock_guard<std::mutex> guard(s_lock);
            // Exact frame tick: the GPU copy must precede this native video
            // sample, not be an old screen/menu frame from a previous tick.
            if (!s_frameReady || s_sequence<=s_lastSequence ||
                s_frameStamp!=s_samples.load()) {
                ++s_fallbacks;
                return false;
            }
            frame.swap(s_bgra);
            s_lastSequence=s_sequence;
            s_frameReady=false;
        }
        if(frame.size()!=static_cast<size_t>(w)*h*4) {
            ++s_fallbacks;
            return false;
        }
        ConvertNV12(frame,w,h,nv12);
        ++s_encodes;
        if(s_encodes<=3 || s_encodes%120==0)
            logging::InfoF("[EVER1] DIRECT GPU FRAME -> NVENC {}, sample {}, {}x{}",
                s_encodes,s_samples.load(),w,h);
        return true;
    }
    void End() {
        if(!s_active.exchange(false))return;
        std::lock_guard<std::mutex> guard(s_lock);
        logging::InfoF("[EVER1] Finished: native samples={}, direct GPU captures={}, encoded GPU frames={}, native NV12 fallbacks={}, selected={}",
            s_samples.load(),s_copies,s_encodes,s_fallbacks,s_selected);
        for(unsigned i=0;i<s_targetCount;++i) {
            const auto& t=s_targets[i];
            logging::InfoF("[EVER1] Candidate {}: {}x{}, format={}, binds={}, name={}",
                t.index,t.width,t.height,static_cast<unsigned>(t.format),
                t.binds,t.name.empty()?"<unnamed>":t.name.c_str());
        }
        ClearTargets();
        s_bgra.clear();
    }
}
static HookFunction initEverOnePass([]() {
    EverOnePass::Initialize();
});
