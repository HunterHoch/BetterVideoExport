#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "EveDx11Capture.h"
#include "Config.h"
#include "Logging.h"
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
    using Present_t = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using OMSetRT_t = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                                ID3D11RenderTargetView* const*,
                                                ID3D11DepthStencilView*);
    Present_t s_origPresent = nullptr;
    OMSetRT_t s_origOM = nullptr;
    std::atomic<bool> s_initialized{false};
    std::atomic<bool> s_active{false};
    std::atomic<bool> s_request{false};
    std::atomic<unsigned> s_width{0}, s_height{0};
    std::atomic<unsigned> s_presentCount{0}, s_eligibleCount{0}, s_capturedCount{0};
    std::atomic<unsigned> s_fallbackCount{0};
    std::atomic<bool> s_debugCapture{false};
    std::mutex s_mutex;
    IDXGISwapChain* s_swap = nullptr; // AddRef'd, owned until process exit.
    std::vector<std::uint8_t> s_nv12;
    bool s_ready = false;
    std::wstring s_output;
    thread_local bool s_inCapture = false;

    static unsigned char Clamp(int v) { return static_cast<unsigned char>(v < 0 ? 0 : v > 255 ? 255 : v); }

    bool Replace(void** slot, void* replacement, void** previous)
    {
        if (!slot || !*slot || !replacement || !previous) return false;
        DWORD protect = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &protect))
            return false;
        *previous = InterlockedExchangePointer(
            reinterpret_cast<void* volatile*>(slot), replacement);
        DWORD unused = 0;
        VirtualProtect(slot, sizeof(void*), protect, &unused);
        return true;
    }

    // 32-bit BMP diagnostic. This image is exactly what the DXGI backbuffer
    // contained after Present(TEST), before any CPU NV12 conversion.
    void WriteBmp(const std::wstring& path, const std::vector<std::uint8_t>& bgra,
                  unsigned w, unsigned h)
    {
        if (bgra.size() != std::size_t(w) * h * 4 || w == 0 || h == 0) return;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        const std::uint32_t size = 54 + static_cast<std::uint32_t>(bgra.size());
        std::uint8_t hdr[54]{};
        hdr[0] = 'B'; hdr[1] = 'M';
        std::memcpy(hdr + 2, &size, 4);
        const std::uint32_t offset = 54, infoSize = 40;
        const std::int32_t width = static_cast<std::int32_t>(w);
        const std::int32_t height = -static_cast<std::int32_t>(h); // top-down BMP
        const std::uint16_t planes = 1, bits = 32;
        std::memcpy(hdr + 10, &offset, 4);
        std::memcpy(hdr + 14, &infoSize, 4);
        std::memcpy(hdr + 18, &width, 4);
        std::memcpy(hdr + 22, &height, 4);
        std::memcpy(hdr + 26, &planes, 2);
        std::memcpy(hdr + 28, &bits, 2);
        DWORD written = 0;
        WriteFile(file, hdr, sizeof(hdr), &written, nullptr);
        WriteFile(file, bgra.data(), static_cast<DWORD>(bgra.size()), &written, nullptr);
        CloseHandle(file);
    }

    // Convert GPU RGBA/BGRA into the exact tightly-packed NV12 layout our
    // existing Media Foundation/FFmpeg shim consumes. BT.709 limited range.
    void ConvertNV12(const std::vector<std::uint8_t>& bgra, unsigned w, unsigned h,
                     std::vector<std::uint8_t>& dst)
    {
        dst.resize(std::size_t(w) * h * 3 / 2);
        unsigned char* Y = dst.data();
        unsigned char* UV = dst.data() + std::size_t(w) * h;
        for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
        {
            const std::size_t p = (std::size_t(y) * w + x) * 4;
            const int b = bgra[p], g = bgra[p + 1], r = bgra[p + 2];
            Y[std::size_t(y) * w + x] = Clamp(16 + ((47*r + 157*g + 16*b + 128) >> 8));
        }
        for (unsigned y = 0; y < h; y += 2)
        for (unsigned x = 0; x < w; x += 2)
        {
            int r = 0, g = 0, b = 0;
            for (unsigned dy = 0; dy < 2; ++dy)
            for (unsigned dx = 0; dx < 2; ++dx)
            {
                const std::size_t p = (std::size_t(y + dy) * w + x + dx) * 4;
                b += bgra[p]; g += bgra[p + 1]; r += bgra[p + 2];
            }
            r = (r + 2) >> 2; g = (g + 2) >> 2; b = (b + 2) >> 2;
            const std::size_t q = std::size_t(y/2) * w + x;
            UV[q]     = Clamp(128 + ((-26*r - 87*g + 113*b + 128) >> 8));
            UV[q + 1] = Clamp(128 + ((113*r - 103*g - 10*b + 128) >> 8));
        }
    }

    // The requested export dimensions must match the swapchain backbuffer.
    // This avoids *pretending* a 1440p GPU frame was captured when the display
    // swapchain was only 1080p or resized underneath us.
    void Capture(ID3D11DeviceContext* ctx)
    {
        if (s_inCapture || !s_active.load() || !s_request.exchange(false)) return;
        s_inCapture = true;
        IDXGISwapChain* swap = nullptr;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            swap = s_swap;
            if (swap) swap->AddRef();
        }
        if (!swap)
        {
            static bool warned = false;
            if (!warned) {
                warned = true;
                logging::Warning("[EVE] No observed DXGI Present yet; use original GTA frames until detected");
            }
            s_request.store(true);
            s_inCapture = false;
            return;
        }

        const unsigned width = s_width.load(), height = s_height.load();
        ID3D11Texture2D* backBuffer = nullptr;
        ID3D11Device* device = nullptr;
        ID3D11Texture2D* staging = nullptr;
        bool success = false;
        DXGI_SWAP_CHAIN_DESC swapDesc{};
        if (SUCCEEDED(swap->GetDesc(&swapDesc)) &&
            SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                     reinterpret_cast<void**>(&backBuffer))) && backBuffer &&
            SUCCEEDED(swap->GetDevice(__uuidof(ID3D11Device),
                     reinterpret_cast<void**>(&device))) && device)
        {
            D3D11_TEXTURE2D_DESC desc{};
            backBuffer->GetDesc(&desc);
            const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                              desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                              desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            if (desc.Width == width && desc.Height == height &&
                (bgra || rgba) && desc.SampleDesc.Count == 1 &&
                (width % 2 == 0) && (height % 2 == 0))
            {
                // EVE calls this at a render-target transition, allowing
                // ReShade/ENB presentation hooks to process the current image.
                // TEST does not swap the displayed frame and adds no scene draw.
                swap->Present(0, DXGI_PRESENT_TEST);
                desc.Usage = D3D11_USAGE_STAGING;
                desc.BindFlags = 0;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                desc.MiscFlags = 0;
                if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)) && staging)
                {
                    ctx->CopyResource(staging, backBuffer);
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
                    {
                        std::vector<std::uint8_t> pixels(std::size_t(width) * height * 4);
                        for (unsigned row = 0; row < height; ++row)
                        {
                            const auto* p = static_cast<const std::uint8_t*>(mapped.pData) +
                                            std::size_t(row) * mapped.RowPitch;
                            auto* out = pixels.data() + std::size_t(row) * width * 4;
                            for (unsigned x = 0; x < width; ++x)
                            {
                                if (bgra)
                                    std::memcpy(out + x*4, p + x*4, 4);
                                else
                                {
                                    out[x*4] = p[x*4 + 2]; out[x*4+1] = p[x*4+1];
                                    out[x*4+2] = p[x*4]; out[x*4+3] = p[x*4+3];
                                }
                            }
                        }
                        ctx->Unmap(staging, 0);
                        std::vector<std::uint8_t> nv12;
                        ConvertNV12(pixels, width, height, nv12);
                        std::wstring debugFile;
                        bool debug = false;
                        {
                            std::lock_guard<std::mutex> lock(s_mutex);
                            if (s_active.load())
                            {
                                s_nv12.swap(nv12);
                                s_ready = true;
                                debug = s_debugCapture.exchange(false);
                                if (debug) debugFile = s_output + L".eve-gpu.bmp";
                                success = true;
                            }
                        }
                        if (debug)
                        {
                            WriteBmp(debugFile, pixels, width, height);
                            logging::Info("[EVE] Saved first post-Present(TEST) backbuffer BMP alongside video");
                        }
                    }
                }
            }
            else
            {
                static bool mismatchLogged = false;
                if (!mismatchLogged)
                {
                    mismatchLogged = true;
                    logging::WarningF("[EVE] DXGI backbuffer {}x{} format={} samples={} doesn't match export {}x{}; original NV12 preserved",
                        desc.Width, desc.Height, static_cast<unsigned>(desc.Format),
                        desc.SampleDesc.Count, width, height);
                }
            }
        }

        if (staging) staging->Release();
        if (device) device->Release();
        if (backBuffer) backBuffer->Release();
        swap->Release();
        if (success)
        {
            const unsigned n = ++s_capturedCount;
            if (n <= 4 || n % 120 == 0)
                logging::InfoF("[EVE] GPU backbuffer captured (frame {}, {}x{})", n, width, height);
        }
        else s_request.store(true);
        s_inCapture = false;
    }

    HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swap, UINT interval, UINT flags)
    {
        if (!s_inCapture && !(flags & DXGI_PRESENT_TEST) && swap)
        {
            ++s_presentCount;
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!s_swap)
            {
                swap->AddRef();
                s_swap = swap;
                logging::Info("[EVE] Game DXGI swapchain observed through Present hook");
            }
        }
        return s_origPresent(swap, interval, flags);
    }

    void STDMETHODCALLTYPE HookOM(ID3D11DeviceContext* ctx, UINT views,
                                  ID3D11RenderTargetView* const* rtvs,
                                  ID3D11DepthStencilView* depth)
    {
        if (s_active.load() && s_request.load() && !s_inCapture &&
            views && rtvs && rtvs[0] && ctx)
        {
            // Like EVE, capture at the transition to an output-size render
            // target. The first matching one is provisional because this
            // build does not yet have a build-3788 signature for "VideoEncode".
            // BMP output is the ground-truth visual check for that ambiguity.
            ID3D11Resource* resource = nullptr;
            rtvs[0]->GetResource(&resource);
            if (resource)
            {
                ID3D11Texture2D* texture = nullptr;
                if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(&texture))) && texture)
                {
                    D3D11_TEXTURE2D_DESC desc{};
                    texture->GetDesc(&desc);
                    if (desc.Width == s_width.load() && desc.Height == s_height.load())
                    {
                        ++s_eligibleCount;
                        Capture(ctx);
                    }
                    texture->Release();
                }
                resource->Release();
            }
        }
        s_origOM(ctx, views, rtvs, depth);
    }

    bool Install()
    {
        // Resolve the process's D3D11 vtables with a disposable DX11 device.
        // This uses atomic vtable swaps rather than MinHook's failing global
        // thread-suspension path. The original methods are always called.
        HWND window = CreateWindowExW(0, L"STATIC", L"BVE_EVE_DX11", WS_POPUP,
                                     0, 0, 16, 16, nullptr, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
        if (!window) { logging::Warning("[EVE] Could not create temporary DXGI probe window"); return false; }
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 1;
        desc.BufferDesc.Width = 16; desc.BufferDesc.Height = 16;
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = window;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        IDXGISwapChain* swap = nullptr;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc,
            &swap, &device, &level, &ctx);
        if (FAILED(hr) || !swap || !ctx)
        {
            logging::WarningF("[EVE] D3D11 vtable probe failed HRESULT 0x{:08X}", static_cast<unsigned>(hr));
            if (ctx) ctx->Release();
            if (device) device->Release();
            if (swap) swap->Release();
            DestroyWindow(window);
            return false;
        }
        auto** swapVt = *reinterpret_cast<void***>(swap);
        auto** ctxVt = *reinterpret_cast<void***>(ctx);
        void* originalPresent = nullptr;
        void* originalOM = nullptr;
        const bool present = Replace(swapVt + 8, reinterpret_cast<void*>(&HookPresent),
                                     &originalPresent);
        const bool om = Replace(ctxVt + 33, reinterpret_cast<void*>(&HookOM), &originalOM);
        s_origPresent = reinterpret_cast<Present_t>(originalPresent);
        s_origOM = reinterpret_cast<OMSetRT_t>(originalOM);
        logging::InfoF("[EVE] DX11 global vtable instrumentation: Present={}, OMSetRenderTargets={}",
                       present, om);
        // If only one is installed, hooks still delegate safely to originals.
        ctx->Release(); device->Release(); swap->Release(); DestroyWindow(window);
        return present && om;
    }
}

namespace EveDx11Capture
{
    void Initialize()
    {
        if (s_initialized.exchange(true)) return;
        if (Config::GetUInt(L"Capture", L"EveDx11", 0) == 0)
        {
            logging::Info("[EVE] DX11 capture disabled (enable [Capture] EveDx11=1)");
            return;
        }
        logging::Info("[EVE] Initializing EVE-style GPU backbuffer capture");
        Install();
    }

    void Start(unsigned w, unsigned h, const std::wstring& videoFile)
    {
        if (Config::GetUInt(L"Capture", L"EveDx11", 0) == 0) return;
        if (!s_origPresent || !s_origOM || (w & 1) || (h & 1) || !w || !h) return;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_output = videoFile;
            s_nv12.clear();
            s_ready = false;
        }
        s_width = w; s_height = h;
        s_presentCount = 0; s_eligibleCount = 0; s_capturedCount = 0; s_fallbackCount = 0;
        s_debugCapture = true;
        s_request = true;
        s_active = true;
        logging::InfoF("[EVE] Armed {}x{} EVE DX11 capture; waiting for render target and Present", w, h);
    }

    bool Consume(std::vector<std::uint8_t>& nv12)
    {
        if (!s_active.load()) return false;
        bool got = false;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_ready)
            {
                nv12.swap(s_nv12);
                s_ready = false;
                got = true;
            }
        }
        s_request = true; // request next GPU frame on a later render-target switch
        if (!got)
        {
            unsigned fallback = ++s_fallbackCount;
            if (fallback <= 4 || fallback % 120 == 0)
                logging::WarningF("[EVE] No GPU frame in time, using GTA NV12 (count={})", fallback);
        }
        return got;
    }

    void Stop()
    {
        if (!s_active.exchange(false)) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_ready = false; s_nv12.clear(); s_request = false;
        logging::InfoF("[EVE] Export ended: Present calls={}, output-size RT binds={}, GPU captures={}, fallback NV12 frames={}",
            s_presentCount.load(), s_eligibleCount.load(),
            s_capturedCount.load(), s_fallbackCount.load());
    }
}
static HookFunction hookEveCapture([]()
{
    EveDx11Capture::Initialize();
});
