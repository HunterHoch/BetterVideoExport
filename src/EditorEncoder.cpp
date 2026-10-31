#include "EditorEncoder.h"

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <string>
#include <vector>

#include <MinHook.h>
#include <Hooking.h>
#include "GameVersion.h"
#include "Logging.h"
#include "Config.h"
#include "ImportHook.h"
#include "EditorTimecycleBlur.h"
#include "EditorExtraTimecycle.h"
#include "EveDx11Capture.h"
#include "NativeBakeBridgeApi.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace EditorEncoder
{
    // Loaded from BetterVideoExport.ini [Encoder] in Initialize().
    static std::wstring kVideoEncodeArgs = L"-c:v libx264 -preset medium -crf 16 -pix_fmt yuv420p";
    static std::wstring kAudioEncodeArgs = L"-c:a aac -b:a 320k";

    static std::wstring g_ffmpegPath;   // empty => feature off, pass through to stock encoder
    static bool g_warnedNoFFmpeg = false;

    static std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }

    static bool EndsWithI(const std::wstring& s, const wchar_t* suffix)
    {
        size_t n = wcslen(suffix);
        if (s.size() < n) return false;
        return _wcsnicmp(s.c_str() + s.size() - n, suffix, n) == 0;
    }

    static bool WriteAll(HANDLE h, const BYTE* p, DWORD n)
    {
        while (n)
        {
            DWORD wrote = 0;
            if (!WriteFile(h, p, n, &wrote, nullptr) || wrote == 0)
                return false;
            p += wrote;
            n -= wrote;
        }
        return true;
    }

    static HANDLE SpawnFfmpeg(std::wstring cmdLine, HANDLE hStdIn, HANDLE logFile)
    {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hStdIn;
        si.hStdOutput = logFile;
        si.hStdError = logFile;

        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
        buf.push_back(L'\0');

        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            logging::ErrorF("[EditorEncoder] CreateProcess(ffmpeg) failed: {}", GetLastError());
            return nullptr;
        }
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    class ShimSinkWriter : public IMFSinkWriter
    {
    public:
        explicit ShimSinkWriter(LPCWSTR url) : m_finalUrl(url) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSinkWriter))
            {
                *ppv = static_cast<IMFSinkWriter*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
        ULONG STDMETHODCALLTYPE Release() override
        {
            ULONG r = InterlockedDecrement(&m_ref);
            if (r == 0) { Cleanup(); delete this; }
            return r;
        }

        HRESULT STDMETHODCALLTYPE AddStream(IMFMediaType* target, DWORD* pIndex) override
        {
            GUID major = GUID_NULL;
            if (target) target->GetGUID(MF_MT_MAJOR_TYPE, &major);
            DWORD idx = m_nextIndex++;
            if (major == MFMediaType_Audio) m_audioStream = (int)idx;
            else                            m_videoStream = (int)idx; // default to video
            if (pIndex) *pIndex = idx;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetInputMediaType(DWORD idx, IMFMediaType* input, IMFAttributes*) override
        {
            if (!input) return S_OK;
            if ((int)idx == m_videoStream)
            {
                MFGetAttributeSize(input, MF_MT_FRAME_SIZE, &m_w, &m_h);
                MFGetAttributeRatio(input, MF_MT_FRAME_RATE, &m_fpsNum, &m_fpsDen);
                GUID subtype = GUID_NULL;
                input->GetGUID(MF_MT_SUBTYPE, &subtype);
                logging::InfoF("[EditorEncoder] Video media type: {}x{} @ {}/{}, NV12={}, stream={}",
                    m_w, m_h, m_fpsNum, m_fpsDen, subtype == MFVideoFormat_NV12, idx);
            }
            else if ((int)idx == m_audioStream)
            {
                input->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &m_sampleRate);
                input->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &m_channels);
                input->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &m_bits);
                logging::InfoF("[EditorEncoder] Audio media type: {}Hz, {} channels, {} bits, stream={}",
                    m_sampleRate, m_channels, m_bits, idx);
            }
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE BeginWriting() override
        {
            logging::InfoF("[EditorEncoder] BeginWriting: video={}x{}, fps={}/{}, audio stream={}", m_w, m_h, m_fpsNum, m_fpsDen, m_audioStream);
            if (m_w == 0 || m_h == 0 || m_fpsNum == 0)
            {
                logging::ErrorF("[EditorEncoder] missing video format (w={} h={} fps={}/{})", m_w, m_h, m_fpsNum, m_fpsDen);
                return E_FAIL;
            }

            const bool haveAudio = (m_audioStream >= 0);
            m_videoOut = haveAudio ? (m_finalUrl + L".tmpvideo.mp4") : m_finalUrl;

            SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
            m_log = CreateFileW((m_finalUrl + L".ffmpeg.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

            HANDLE rd = nullptr;
            if (!CreatePipe(&rd, &m_videoPipe, &sa, 0))
            {
                logging::ErrorF("[EditorEncoder] CreatePipe failed: {}", GetLastError());
                return E_FAIL;
            }
            SetHandleInformation(m_videoPipe, HANDLE_FLAG_INHERIT, 0);

            std::wstring cmd = L"\"" + g_ffmpegPath + L"\" -y -hide_banner -loglevel error"
                L" -f rawvideo -pixel_format nv12"
                L" -video_size " + std::to_wstring(m_w) + L"x" + std::to_wstring(m_h) +
                L" -framerate " + std::to_wstring(m_fpsNum) + L"/" + std::to_wstring(m_fpsDen) +
                L" -i pipe:0 " + kVideoEncodeArgs + L" \"" + m_videoOut + L"\"";

            logging::InfoF("[EditorEncoder] FFmpeg command: {}", Narrow(cmd));
            m_ffmpeg = SpawnFfmpeg(cmd, rd, m_log);
            CloseHandle(rd); // child owns its copy
            if (!m_ffmpeg)
            {
                CloseHandle(m_videoPipe); m_videoPipe = nullptr;
                return E_FAIL;
            }
            logging::InfoF("[EditorEncoder] encoding {}x{} @ {}/{} fps -> {}", m_w, m_h, m_fpsNum, m_fpsDen, Narrow(m_finalUrl));
            logging::InfoF("[EditorEncoder] FFmpeg stderr: {}", Narrow(m_finalUrl + L".ffmpeg.log"));
            m_blurSession = true;
            EditorTimecycleBlur::SetExportActive(true);
            EditorExtraTimecycle::Begin();
            EveDx11Capture::Start(m_w, m_h, m_finalUrl);
            NativeBakeBridge::Begin(m_w, m_h);

            if (haveAudio)
                OpenWav();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE WriteSample(DWORD idx, IMFSample* sample) override
        {
            if (m_failed) return E_FAIL;
            if (!sample) return S_OK;

            IMFMediaBuffer* buf = nullptr;
            if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || !buf)
                return S_OK;

            BYTE* data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            HRESULT hr = buf->Lock(&data, &maxLen, &curLen);
            if (SUCCEEDED(hr))
            {
                if ((int)idx == m_videoStream && m_videoPipe)
                {
                    NativeBakeBridge::OnVideoSample();
                    EditorExtraTimecycle::NextFrame();
                    if (!m_loggedFirstVideo)
                    {
                        m_loggedFirstVideo = true;
                        const unsigned long long expected = (unsigned long long)m_w * m_h * 3 / 2;
                        logging::InfoF("[EditorEncoder] First NV12 sample: {} bytes, expected {} bytes", curLen, expected);
                        if (curLen != expected)
                            logging::Warning("[EditorEncoder] Frame length differs from tightly packed NV12. FFmpeg may reject these samples.");

                        // Diagnostic capture is the exact pre-FFmpeg input, not a screenshot
                        // of the playback window. One frame only; never changes the video.
                        if (Config::GetUInt(L"Diagnostics", L"DumpFirstNV12", 0) != 0 &&
                            curLen == expected)
                        {
                            const std::wstring framePath = m_finalUrl + L".first-frame.nv12";
                            HANDLE frameFile = CreateFileW(framePath.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (frameFile != INVALID_HANDLE_VALUE)
                            {
                                const bool saved = WriteAll(frameFile, data, curLen);
                                CloseHandle(frameFile);
                                if (saved)
                                    logging::InfoF("[EditorEncoder] Pre-encoder frame dump saved: {} ({}x{}, NV12)",
                                        Narrow(framePath), m_w, m_h);
                                else
                                    logging::Warning("[EditorEncoder] Failed to save complete pre-encoder NV12 frame");
                            }
                            else
                                logging::WarningF("[EditorEncoder] Failed to create NV12 frame dump, Win32 error={}",
                                    GetLastError());
                        }
                    }
                    // Use a GPU frame that has ACTUALLY passed through
                    // ReShade's render_effects() during Rockstar's bake.
                    // The previous implementation applied effects to GPU
                    // memory, then always encoded the old GTA NV12 input.
                    std::vector<std::uint8_t> processed;
                    const bool bakedFx = NativeBakeBridge::ConsumeNV12(processed, m_w, m_h) &&
                                         processed.size() == curLen;
                    const bool legacyEve = !bakedFx &&
                        EveDx11Capture::Consume(processed) && processed.size() == curLen;
                    const BYTE* input = (bakedFx || legacyEve) ? processed.data() : data;
                    if (!WriteAll(m_videoPipe, input, curLen))
                    {
                        logging::Error("[EditorEncoder] video pipe write failed (ffmpeg died?)");
                        m_failed = true;
                    }
                }
                else if ((int)idx == m_audioStream && m_wav)
                {
                    DWORD wrote = 0;
                    WriteFile(m_wav, data, curLen, &wrote, nullptr);
                    m_wavBytes += curLen;
                }
                buf->Unlock();
            }
            buf->Release();
            if (m_failed)
                logging::Error("[EditorEncoder] WriteSample returning E_FAIL; inspect *.ffmpeg.log alongside the output.");
            return m_failed ? E_FAIL : S_OK;
        }

        HRESULT STDMETHODCALLTYPE Finalize() override
        {
            ClosePipeAndWait();      // flush video, let ffmpeg finish encoding
            if (m_wav) { CloseWav(); MuxAudio(); }
            Cleanup();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE Flush(DWORD) override
        {
            if (m_ffmpeg) { TerminateProcess(m_ffmpeg, 1); }
            ClosePipeAndWait();
            if (m_wav) CloseWav();
            DeleteFileW(m_videoOut.c_str());
            DeleteFileW(m_wavPath.c_str());
            Cleanup();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SendStreamTick(DWORD, LONGLONG) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE PlaceMarker(DWORD, LPVOID) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE NotifyEndOfSegment(DWORD) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE GetServiceForStream(DWORD, REFGUID, REFIID, LPVOID* ppv) override
        {
            if (ppv) *ppv = nullptr;
            return MF_E_UNSUPPORTED_SERVICE; // GTA only uses this for optional metadata
        }
        HRESULT STDMETHODCALLTYPE GetStatistics(DWORD, MF_SINK_WRITER_STATISTICS*) override { return E_NOTIMPL; }

    private:
        void OpenWav()
        {
            m_wavPath = m_finalUrl + L".tmpaudio.wav";
            m_wav = CreateFileW(m_wavPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_wav == INVALID_HANDLE_VALUE) { m_wav = nullptr; return; }
            BYTE hdr[44] = {};
            DWORD w = 0;
            WriteFile(m_wav, hdr, sizeof(hdr), &w, nullptr);
        }

        void CloseWav()
        {
            if (!m_wav) return;
            const uint32_t blockAlign = m_channels * (m_bits / 8);
            const uint32_t byteRate = m_sampleRate * blockAlign;
            const uint32_t dataSize = m_wavBytes;
            const uint32_t riffSize = 36 + dataSize;

            BYTE h[44];
            memcpy(h + 0, "RIFF", 4);
            memcpy(h + 4, &riffSize, 4);
            memcpy(h + 8, "WAVE", 4);
            memcpy(h + 12, "fmt ", 4);
            uint32_t fmtSize = 16;
            memcpy(h + 16, &fmtSize, 4);
            uint16_t pcm = 1;
            memcpy(h + 20, &pcm, 2);
            uint16_t ch = (uint16_t)m_channels;
            memcpy(h + 22, &ch, 2);
            memcpy(h + 24, &m_sampleRate, 4);
            memcpy(h + 28, &byteRate, 4);
            uint16_t ba = (uint16_t)blockAlign; 
            memcpy(h + 32, &ba, 2);
            uint16_t bits = (uint16_t)m_bits;
            memcpy(h + 34, &bits, 2);
            memcpy(h + 36, "data", 4);
            memcpy(h + 40, &dataSize, 4);

            SetFilePointer(m_wav, 0, nullptr, FILE_BEGIN);
            DWORD w = 0; WriteFile(m_wav, h, sizeof(h), &w, nullptr);
            CloseHandle(m_wav); m_wav = nullptr;
        }

        void MuxAudio()
        {
            std::wstring cmd = L"\"" + g_ffmpegPath + L"\" -y -hide_banner -loglevel error"
                L" -i \"" + m_videoOut + L"\" -i \"" + m_wavPath + L"\""
                L" -c:v copy " + kAudioEncodeArgs + L" -shortest \"" + m_finalUrl + L"\"";
            HANDLE p = SpawnFfmpeg(cmd, nullptr, m_log);
            if (p)
            {
                WaitForSingleObject(p, INFINITE);
                CloseHandle(p);
                DeleteFileW(m_videoOut.c_str());
                DeleteFileW(m_wavPath.c_str());
            }
        }

        void ClosePipeAndWait()
        {
            if (m_videoPipe) { CloseHandle(m_videoPipe); m_videoPipe = nullptr; }
            if (m_ffmpeg)
            {
                WaitForSingleObject(m_ffmpeg, INFINITE);
                DWORD exitCode = STILL_ACTIVE;
                if (GetExitCodeProcess(m_ffmpeg, &exitCode))
                    logging::InfoF("[EditorEncoder] FFmpeg video process exited: {}", exitCode);
                if (exitCode != 0)
                    logging::ErrorF("[EditorEncoder] Video encoder failed (exit {}). Read the .ffmpeg.log next to your output.", exitCode);
                CloseHandle(m_ffmpeg); m_ffmpeg = nullptr;
            }
        }

        void Cleanup()
        {
            if (m_blurSession)
            {
                m_blurSession = false;
                NativeBakeBridge::End();
                EveDx11Capture::Stop();
                EditorExtraTimecycle::End();
                EditorTimecycleBlur::SetExportActive(false);
            }
            if (m_log && m_log != INVALID_HANDLE_VALUE) { CloseHandle(m_log); m_log = nullptr; }
        }

        LONG m_ref = 1;
        std::wstring m_finalUrl, m_videoOut, m_wavPath;
        int m_videoStream = -1, m_audioStream = -1;
        DWORD m_nextIndex = 0;
        UINT32 m_w = 0, m_h = 0, m_fpsNum = 0, m_fpsDen = 1;
        UINT32 m_sampleRate = 48000, m_channels = 2, m_bits = 16;
        UINT32 m_wavBytes = 0;
        HANDLE m_videoPipe = nullptr, m_ffmpeg = nullptr, m_wav = nullptr, m_log = nullptr;
        bool m_failed = false;
        bool m_loggedFirstVideo = false;
        bool m_blurSession = false;
    };

    using MFCreateSinkWriterFromURL_t = HRESULT(STDAPICALLTYPE*)(LPCWSTR, IMFByteStream*, IMFAttributes*, IMFSinkWriter**);
    static MFCreateSinkWriterFromURL_t g_origCreate = nullptr;

    static HRESULT STDAPICALLTYPE Hooked_Create(LPCWSTR url, IMFByteStream* bs, IMFAttributes* attr, IMFSinkWriter** out)
    {
        logging::InfoF("[EditorEncoder] Sink writer requested: {}", Narrow(url ? std::wstring(url) : std::wstring(L"<null>")));
        if (url && EndsWithI(url, L".mp4"))
        {
            logging::InfoF("[EditorEncoder] MP4 export requested: {}", Narrow(url));
            if (out && !g_ffmpegPath.empty())
            {
                logging::Info("[EditorEncoder] Using FFmpeg sink writer");
                *out = new ShimSinkWriter(url);
                return S_OK;
            }
            logging::Error("[EditorEncoder] FFmpeg is unavailable: FALLING BACK to GTA's native H.264 encoder. 4K60 may fail.");
        }
        const HRESULT hr = g_origCreate(url, bs, attr, out);
        if (FAILED(hr))
            logging::ErrorF("[EditorEncoder] Native MFCreateSinkWriterFromURL failed HRESULT=0x{:08X}", (unsigned int)hr);
        return hr;
    }

    static std::wstring FindFfmpeg()
    {
        // FiveM often runs from FiveM.app, not the GTA V installation folder.
        // An explicit path is preferred to avoid silently switching to native H.264.
        const std::wstring configured = Config::GetStr(L"Encoder", L"FFmpegPath", L"");
        if (!configured.empty())
        {
            if (GetFileAttributesW(configured.c_str()) != INVALID_FILE_ATTRIBUTES)
                return configured;
            logging::ErrorF("[EditorEncoder] FFmpegPath does not exist: {}", Narrow(configured));
            return {};
        }

        wchar_t localAppData[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH))
        {
            const std::wstring plugins = std::wstring(localAppData) + L"\\FiveM\\FiveM.app\\plugins\\ffmpeg.exe";
            if (GetFileAttributesW(plugins.c_str()) != INVALID_FILE_ATTRIBUTES)
                return plugins;
        }

        wchar_t exe[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH))
        {
            std::wstring dir(exe);
            size_t slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
            {
                std::wstring cand = dir.substr(0, slash + 1) + L"ffmpeg.exe";
                if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return cand;
            }
        }

        wchar_t found[MAX_PATH];
        if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr))
            return found;
        return {};
    }

    void Initialize()
    {
        if (!gameversion::IsLegacy())
            return;

        Config::EnsureDefaultFile();
        kVideoEncodeArgs = Config::GetStr(L"Encoder", L"VideoArgs", kVideoEncodeArgs.c_str());
        kAudioEncodeArgs = Config::GetStr(L"Encoder", L"AudioArgs", kAudioEncodeArgs.c_str());

        logging::InfoF("[EditorEncoder] Active INI path: {}", Narrow(Config::IniPath()));
        g_ffmpegPath = FindFfmpeg();
        if (g_ffmpegPath.empty())
        {
            logging::Error("[EditorEncoder] FFmpeg NOT FOUND. Place ffmpeg.exe in FiveM.app\\plugins, set [Encoder] FFmpegPath=, or add it to PATH. Native encoder fallback will likely fail at 4K60.");
            return;
        }
        logging::InfoF("[EditorEncoder] using ffmpeg: {}", Narrow(g_ffmpegPath));

        HMODULE mf = LoadLibraryW(L"mfreadwrite.dll");
        void* target = mf ? (void*)GetProcAddress(mf, "MFCreateSinkWriterFromURL") : nullptr;
        if (!target)
        {
            logging::Error("[EditorEncoder] MFCreateSinkWriterFromURL not found");
            return;
        }

        // Prefer an IAT hook. MinHook's global thread freeze has returned
        // MH_ERROR_MEMORY_ALLOC (9) inside FiveM's highly threaded subprocess.
        // An import-pointer swap does not suspend the game's threads.
        void* directOriginal = nullptr;
        std::string iatDiagnostic;
        if (ImportHook::Patch(GetModuleHandleW(nullptr), "mfreadwrite.dll",
                              "MFCreateSinkWriterFromURL", target,
                              (void*)&Hooked_Create, &directOriginal, iatDiagnostic))
        {
            g_origCreate = reinterpret_cast<MFCreateSinkWriterFromURL_t>(directOriginal);
            logging::InfoF("[EditorEncoder] IAT hook ACTIVE: {}", iatDiagnostic);
            return;
        }
        logging::WarningF("[EditorEncoder] IAT hook unavailable: {}; attempting MinHook fallback",
                          iatDiagnostic);

        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            logging::ErrorF("[EditorEncoder] MinHook initialization failed: {}",
                            MH_StatusToString(init));
            return;
        }
        const MH_STATUS creation = MH_CreateHook(target, (void*)&Hooked_Create, (void**)&g_origCreate);
        if (creation != MH_OK)
        {
            logging::ErrorF("[EditorEncoder] Inline hook creation failed: {}",
                            MH_StatusToString(creation));
            return;
        }
        // A transient thread snapshot failure can surface as status 9.
        // Retry a bounded number of times rather than silently falling back.
        for (int attempt = 1; attempt <= 3; ++attempt)
        {
            const MH_STATUS enabled = MH_EnableHook(target);
            if (enabled == MH_OK || enabled == MH_ERROR_ENABLED)
            {
                logging::InfoF("[EditorEncoder] Inline MinHook ACTIVE (attempt {})", attempt);
                return;
            }
            logging::WarningF("[EditorEncoder] Inline hook enable attempt {} failed: {}",
                              attempt, MH_StatusToString(enabled));
            if (enabled != MH_ERROR_MEMORY_ALLOC)
                break;
            Sleep(25);
        }
        logging::Error("[EditorEncoder] Encoder hook UNAVAILABLE. 4K60 export cannot use FFmpeg.");
    }
}

static HookFunction hookEditorEncoder([]()
{
    EditorEncoder::Initialize();
});
