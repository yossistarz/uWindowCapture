#include <windows.h>
#include <dwmapi.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <cstdio>
#include <stdexcept>
#include <future>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <array>
#include <algorithm>
#include <cstring>
#include <vector>
#include "Thread.h"
#include "Util.h"
#include "Cursor.h"
#include "IconTexture.h"
#include "UnityTextureReference.h"
#include "Unity.h"
#include "IUnityGraphicsD3D11.h"
#include "Message.h"
#include "WindowsGraphicsCapture.h"
#include "WindowManager.h"
#include "WindowTexture.h"
#include "Debug.h"

extern IUnityInterfaces* g_unity;

namespace
{
    // Keep storage alive after the final Release so the old race fails a check rather than crashing the runner.
    struct PausedTexture final : ID3D11Texture2D
    {
        std::atomic<ULONG> references{1};
        std::promise<void> entered, resume;
        bool wasAliveDuringAccess = false;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
        ULONG STDMETHODCALLTYPE Release() override { return --references; }
        void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override { *device = nullptr; }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
        void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* type) override { *type = D3D11_RESOURCE_DIMENSION_TEXTURE2D; }
        void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
        UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
        void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* desc) override
        {
            entered.set_value();
            resume.get_future().wait();
            wasAliveDuringAccess = references.load() != 0;
            *desc = {};
            desc->Width = desc->Height = 4;
        }
    };

    struct TextureLifetime final : IUnknown
    {
        std::atomic<ULONG> references{1};
        std::atomic<int>& destroyed;
        explicit TextureLifetime(std::atomic<int>& count) : destroyed(count) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
        {
            *object = nullptr;
            if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
            *object = this;
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const auto count = --references;
            if (!count) { ++destroyed; delete this; }
            return count;
        }
    };

    constexpr GUID LifetimeKey{0xb61df94d, 0x9380, 0x43cf, {0xa5, 0x11, 0xf7, 0x8a, 0x01, 0x1a, 0x87, 0x32}};
}

struct CursorRegressionTests
{
    inline static int checks = 0;
    inline static int failures = 0;
    inline static Microsoft::WRL::ComPtr<ID3D11Device> textureDevice;

    static void Check(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            fprintf(stderr, "FAIL: %s\n", message);
        }
    }

    static void UNITY_INTERFACE_API NativeError(const char* text)
    {
        ++failures;
        fprintf(stderr, "NATIVE ERROR: %s\n", text);
    }

    static void PrepareDevice()
    {
        winrt::com_ptr<ID3D11Device> device;
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, nullptr));
        textureDevice = device.get();
        auto dxgi = device.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        auto manager = std::make_unique<WindowsGraphicsCaptureManager>();
        manager->deviceWinRt_ = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        WindowManager::Create();
        WindowManager::Get().windowsGraphicsCaptureManager_ = std::move(manager);
    }

    static void RequestDrivenThread()
    {
        std::atomic<int> callbacks{0};
        std::promise<void> ran;
        auto completed = ran.get_future();
        ThreadLoop loop(L"Request-driven regression");
        loop.Wake();
        loop.Start([&]
        {
            if (++callbacks == 1) ran.set_value();
        }, ThreadLoop::microseconds::zero(), ThreadLoop::WaitMode::OnRequest);
        Check(completed.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
            "a request made before worker startup is not lost");
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        Check(callbacks == 1, "idle request-driven worker does not poll");
        const auto beforeStop = std::chrono::steady_clock::now();
        loop.Stop();
        Check(std::chrono::steady_clock::now() - beforeStop < std::chrono::milliseconds(100),
            "stopping an indefinitely waiting worker wakes it promptly");
        loop.Restart();
        loop.Wake();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        loop.Stop();
        Check(callbacks == 2, "restart preserves request-driven mode");
    }

    template<class Owner, class Operation>
    static void ReplaceDuringAccess(Owner& owner, Operation operation, const char* label)
    {
        PausedTexture texture;
        auto entered = texture.entered.get_future();
        owner.SetUnityTexturePtr(&texture);
        Check(texture.references == 2, "native owner retains the published texture");
        Check(owner.GetUnityTexturePtr() == &texture, "borrowed C API getter keeps its existing identity");
        auto worker = std::async(std::launch::async, operation);
        const bool paused = entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        owner.SetUnityTexturePtr(nullptr);
        texture.Release();
        Check(paused, "worker reaches GetDesc before the texture is cleared");
        Check(texture.references == 1, "only the active worker retains the retired texture");
        texture.resume.set_value();
        worker.get();
        Check(texture.wasAliveDuringAccess, "retired texture stays alive inside the real upload/recreation operation");
        Check(texture.references == 0, "last worker releases the retired texture");
        printf("TEXTURE_LIFETIME %s replacement during GetDesc checked\n", label);
    }

    static void LifetimeDuringUpload(HWND handle)
    {
        Window::Data1 data{};
        data.hWnd = handle;
        Window window(3, data);
        WindowTexture texture(&window);
        texture.SetCaptureMode(CaptureMode::BitBlt);
        texture.textureWidth_ = texture.textureHeight_ = 4;
        texture.bufferWidth_ = texture.bufferHeight_ = 4;
        ReplaceDuringAccess(texture, [&] { texture.RecreateSharedTextureIfNeeded(); }, "window");

        Cursor cursor;
        cursor.StopCapture();
        cursor.width_ = cursor.height_ = 4;
        cursor.buffer_.ExpandIfNeeded(4 * 4 * 4);
        cursor.hasCaptured_ = true;
        ReplaceDuringAccess(cursor, [&] { cursor.Upload(); }, "cursor");

        IconTexture icon(&window);
        icon.width_ = icon.height_ = 4;
        icon.buffer_.ExpandIfNeeded(4 * 4 * 4);
        ReplaceDuringAccess(icon, [&] { icon.Upload(); }, "icon");

        RenderDuringReplacement(texture, "window render");
        cursor.hasCaptured_ = true;
        RenderDuringReplacement(cursor, "cursor render");
        RenderDuringReplacement(icon, "icon render");
    }

    template<class Owner>
    static void RenderDuringReplacement(Owner& owner, const char* label)
    {
        owner.sharedTexture_ = CreateTexture(8, 4);
        owner.sharedHandle_ = reinterpret_cast<HANDLE>(1);
        bool rendered = true;
        ReplaceDuringAccess(owner, [&] { rendered = owner.Render(); }, label);
        Check(!rendered, "render skips the old shared size while a replacement is pending");
    }

    static Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateTexture(UINT width, UINT height)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        winrt::check_hresult(textureDevice->CreateTexture2D(&desc, nullptr, &texture));
        return texture;
    }

    static ID3D11Device* UNITY_INTERFACE_API TestDevice() { return textureDevice.Get(); }
    static IUnityInterface* UNITY_INTERFACE_API TestInterface(UnityInterfaceGUID)
    {
        static auto d3d = []
        {
            IUnityGraphicsD3D11 result{};
            result.GetDevice = TestDevice;
            return result;
        }();
        return &d3d;
    }

    static void RenderReadback(HWND handle)
    {
        IUnityInterfaces interfaces{};
        interfaces.GetInterface = TestInterface;
        auto previousUnity = g_unity;
        g_unity = &interfaces;
        MessageManager::Create();
        ScopedReleaser cleanup([&]
        {
            WindowManager::Get().uploadManager_.reset();
            MessageManager::Destroy();
            g_unity = previousUnity;
        });
        auto uploader = std::make_unique<UploadManager>();
        uploader->initThread_.join();
        uploader->StopUploadThread();
        Check(uploader->GetDevice() != nullptr, "real upload D3D device was created");
        WindowManager::Get().uploadManager_ = std::move(uploader);
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        textureDevice->GetImmediateContext(&context);
        Window::Data1 data{};
        data.hWnd = handle;
        Window window(4, data);
        WindowTexture capture(&window);
        capture.SetCaptureMode(CaptureMode::BitBlt);
        for (int i = 0; i < 32; ++i)
        {
            const UINT width = i % 2 == 0 ? 4 : 8;
            auto target = CreateTexture(width, 4);
            capture.SetUnityTexturePtr(target.Get());
            Check(capture.GetRenderedCaptureMode(target.Get()) == CaptureMode::None,
                "allocating/replacing a Unity texture does not publish a valid frame");
            capture.textureWidth_ = capture.bufferWidth_ = width;
            capture.textureHeight_ = capture.bufferHeight_ = 4;
            capture.buffer_.ExpandIfNeeded(width * 4 * 4);
            memset(capture.buffer_.Get(), i + 1, width * 4 * 4);
            if (i != 0)
            {
                const auto messages = MessageManager::Get().GetCount();
                Check(!capture.Render(), "a new target size is not copied from the old shared texture");
                Check(MessageManager::Get().GetCount() == messages, "skipped resize does not announce a captured frame");
            }
            Check(capture.Upload(), "resized window uploads through the real D3D shared texture");
            Check(capture.Render(), "resized window renders to the current owned Unity texture");
            Check(capture.GetRenderedCaptureMode(target.Get()) == CaptureMode::BitBlt,
                "rendered frame identifies its target and actual backend");
            D3D11_TEXTURE2D_DESC desc;
            target->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
            winrt::check_hresult(textureDevice->CreateTexture2D(&desc, nullptr, &staging));
            context->CopyResource(staging.Get(), target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            bool correct = true;
            for (UINT y = 0; y < 4; ++y)
                for (UINT x = 0; x < width * 4; ++x)
                    if (static_cast<BYTE*>(mapped.pData)[y * mapped.RowPitch + x] != i + 1) correct = false;
            context->Unmap(staging.Get(), 0);
            if (!correct) fprintf(stderr, "PIXEL_READBACK replacement=%d width=%u expected=%d\n", i, width, i + 1);
            Check(correct, "GPU readback contains the uploaded pixels after resize/replacement");
        }
        capture.SetUnityTexturePtr(nullptr);
        Check(capture.GetRenderedCaptureMode(nullptr) == CaptureMode::None, "cleared target has no ready frame");
        Check(!capture.Render(), "cleared Unity target is not rendered");
        printf("TEXTURE_GPU_READBACK replacements=32 checked\n");
    }

    static void MipReadbackAndSwitch(HWND handle)
    {
        using Microsoft::WRL::ComPtr;
        IUnityInterfaces interfaces{};
        interfaces.GetInterface = TestInterface;
        auto previousUnity = g_unity;
        g_unity = &interfaces;
        MessageManager::Create();
        ScopedReleaser cleanup([&]
        {
            WindowManager::Get().uploadManager_.reset();
            MessageManager::Destroy();
            g_unity = previousUnity;
        });
        auto uploader = std::make_unique<UploadManager>();
        uploader->initThread_.join(); uploader->StopUploadThread();
        WindowManager::Get().uploadManager_ = std::move(uploader);
        ComPtr<ID3D11DeviceContext> context;
        textureDevice->GetImmediateContext(&context);
        Window::Data1 data{}; data.hWnd = handle;
        Window window(5, data);
        WindowTexture capture(&window);
        capture.SetCaptureMode(CaptureMode::BitBlt);
        capture.textureWidth_ = capture.textureHeight_ = capture.bufferWidth_ = capture.bufferHeight_ = 8;
        capture.buffer_.ExpandIfNeeded(8 * 8 * 4);
        for (int pixel = 0; pixel < 64; ++pixel)
        {
            auto* value = capture.buffer_.Get(pixel * 4);
            value[0] = value[1] = value[2] = pixel % 2 ? 255 : 0;
            value[3] = 255;
        }
        auto mipTarget = [&](bool srgb)
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = 8; desc.MipLevels = 0; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS; desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            ComPtr<ID3D11Texture2D> target;
            winrt::check_hresult(textureDevice->CreateTexture2D(&desc, nullptr, &target));
            capture.SetUnityTexturePtr(target.Get(), 8, 8, srgb);
            return target;
        };
        auto sampleMip = [&](ID3D11Texture2D* target, UINT mip)
        {
            D3D11_TEXTURE2D_DESC desc{};
            target->GetDesc(&desc);
            desc.Width = (std::max)(1u, desc.Width >> mip); desc.Height = (std::max)(1u, desc.Height >> mip);
            desc.MipLevels = desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            winrt::check_hresult(textureDevice->CreateTexture2D(&desc, nullptr, &staging));
            context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, target, mip, nullptr);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            std::array<BYTE, 4> result{};
            memcpy(result.data(), mapped.pData, 4);
            context->Unmap(staging.Get(), 0);
            return result;
        };
        auto first = mipTarget(false);
        Check(capture.Upload() && capture.Render(), "mip capture uploads and publishes through the real shared-device copy");
        auto level = sampleMip(first.Get(), 1);
        Check(level[0] >= 127 && level[0] <= 129 && level[3] == 255, "native generated mip contains averaged pixels and preserved alpha");
        auto base = sampleMip(first.Get(), 0);
        Check(base[0] == 0 && base[3] == 255, "level-zero capture pixels remain unchanged");
        const auto generations = capture.mipmapGenerationCount_;
        const auto messages = MessageManager::Get().GetCount();
        Check(!capture.Render(), "unchanged source does not regenerate mipmaps every render frame");
        Check(capture.mipmapGenerationCount_ == generations && MessageManager::Get().GetCount() == messages,
            "skipped mip work does not falsely publish another capture");
        Check(capture.Upload() && capture.Render() && capture.mipmapGenerationCount_ == generations + 1,
            "a new uploaded version regenerates and publishes its mips");
        auto legacy = CreateTexture(8, 8);
        capture.SetUnityTexturePtr(legacy.Get(), 8, 8);
        Check(capture.GetRenderedCaptureMode(first.Get()) == CaptureMode::BitBlt &&
            capture.GetRenderedCaptureMode(legacy.Get()) == CaptureMode::None,
            "same-size sampling switch retains the last valid target until replacement publication");
        Check(capture.Render() && capture.GetRenderedCaptureMode(legacy.Get()) == CaptureMode::BitBlt,
            "legacy target remains supported by the new native renderer");
        auto srgb = mipTarget(true);
        Check(capture.Render(), "sRGB mip target publishes a valid frame");
        level = sampleMip(srgb.Get(), 1);
        Check(level[0] >= 186 && level[0] <= 190, "declared sRGB view generates gamma-correct mip pixels");
        capture.SetUnityTexturePtr(nullptr);
        Check(!capture.mipView_ && capture.GetRenderedCaptureMode(nullptr) == CaptureMode::None,
            "clearing capture releases the cached mip view and publication");
        printf("MIP_GPU_READBACK mode-switch, UNorm/sRGB, version reuse checked\n");
    }

    static void LifetimeStressAndCost()
    {
        std::atomic<int> destroyed{0}, reads{0}, invalid{0};
        UnityTextureReference current;
        std::atomic<bool> stop{false};
        auto reader = std::async(std::launch::async, [&]
        {
            while (!stop)
            {
                const auto snapshot = current.Acquire();
                if (!snapshot) continue;
                D3D11_TEXTURE2D_DESC desc;
                snapshot->GetDesc(&desc);
                ++reads;
                if (desc.Width < 4 || desc.Width > 11 || desc.Height != 4) ++invalid;
            }
        });
        ScopedReleaser stopReader([&]
        {
            stop = true;
            if (reader.valid()) reader.wait();
        });
        constexpr int replacements = 2000;
        for (int i = 0; i < replacements; ++i)
        {
            auto texture = CreateTexture(4 + i % 8, 4);
            auto tracker = new TextureLifetime(destroyed);
            const auto result = texture->SetPrivateDataInterface(LifetimeKey, tracker);
            tracker->Release();
            winrt::check_hresult(result);
            current.Set(texture.Get());
            texture.Reset();
            if (i % 3 == 0) current.Set(nullptr);
        }
        current.Set(nullptr);
        stop = true;
        reader.get();
        Check(reads > 0 && invalid == 0, "concurrent real-D3D replacement/clear yields valid snapshots");
        Check(destroyed == replacements, "all retired real-D3D textures are released without accumulation");
        printf("TEXTURE_STRESS replacements=%d reads=%d released=%d\n",
            replacements, reads.load(), destroyed.load());

        auto texture = CreateTexture(1920, 1080);
        current.Set(texture.Get());
        std::atomic<ID3D11Texture2D*> borrowed{texture.Get()};
        constexpr int iterations = 200000;
        volatile uint64_t checksum = 0;
        const auto measure = [&](bool owned)
        {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < iterations; ++i)
            {
                D3D11_TEXTURE2D_DESC desc;
                if (owned)
                {
                    const auto snapshot = current.Acquire();
                    snapshot->GetDesc(&desc);
                }
                else borrowed.load()->GetDesc(&desc);
                checksum += desc.Width;
            }
            return std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - start).count() / iterations;
        };
        const double borrowedNs = measure(false), ownedNs = measure(true);
        printf("TEXTURE_COST iterations=%d borrowed_getdesc_ns=%.1f owned_getdesc_ns=%.1f checksum=%llu\n",
            iterations, borrowedNs, ownedNs, static_cast<unsigned long long>(checksum));
        current.Set(nullptr);
    }

    static void SessionSettings(HWND window)
    {
        if (!WindowsGraphicsCapture::IsCursorCaptureEnabledApiSupported())
            throw std::runtime_error("The native cursor regression requires the WGC cursor-control API.");
        auto capture = std::make_shared<WindowsGraphicsCapture>(window);
        Check(capture->IsAvailable(), "owned test window is capturable");
        capture->EnableCursorCapture(false);
        Check(!capture->session_, "cursor exclusion can be requested before a session exists");
        capture->Start();
        if (!capture->IsStarted()) throw std::runtime_error("WGC test session did not start.");
        Check(!capture->session_.IsCursorCaptureEnabled(), "new session applies pre-start cursor exclusion");
        capture->EnableCursorCapture(false);
        Check(!capture->session_.IsCursorCaptureEnabled(), "repeated exclusion agrees with actual session");
        capture->EnableCursorCapture(true);
        Check(capture->session_.IsCursorCaptureEnabled(), "live capture-backed cursor enable works");
        capture->EnableCursorCapture(false);
        Check(!capture->session_.IsCursorCaptureEnabled(), "live local-cursor exclusion works");
        for (int i = 0; i < 3; ++i)
        {
            capture->Stop();
            capture->Start();
            Check(capture->IsStarted(), "idle session starts again");
            Check(!capture->session_.IsCursorCaptureEnabled(), "idle restart retains cursor exclusion");
            capture->Restart();
            Check(!capture->IsStarted(), "capture recovery releases the old session before its bounded retry");
            capture->retryAt_ = 0;
            capture->Start();
            Check(capture->IsStarted(), "capture recovery restarts the session");
            Check(!capture->session_.IsCursorCaptureEnabled(), "capture recovery retains cursor exclusion");
        }
        capture->EnableCursorCapture(true);
        capture->Restart();
        capture->retryAt_ = 0;
        capture->Start();
        Check(capture->session_.IsCursorCaptureEnabled(), "restart also preserves an enabled cursor");
        capture->Stop();
    }

    static void FailedMonitorCaptureRemainsRecoverable()
    {
        const auto& manager = WindowManager::GetWindowsGraphicsCaptureManager();
        const auto capture = manager->Create(reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(1)));
        Check(capture != nullptr, "a failed monitor capture is retained for bounded recovery instead of permanent BitBlt");
        if (capture)
        {
            const auto status = capture->GetStatus();
            Check(FAILED(status.lastError), "the actual failing CreateForMonitor HRESULT is preserved");
            Check(status.failureStage == static_cast<int>(WgcFailureStage::CreateItem), "monitor creation failure has an explicit stage");
            Check(status.itemAttempts == 1 && status.available == 0, "failed creation is observable without publishing WGC readiness");
        }
        if (capture) manager->Destroy(capture);
    }

    static void MonitorRetryAndRecovery(HWND handle)
    {
        WindowsGraphicsCapture::createItemFailureForTests_ = [] { return E_ACCESSDENIED; };
        ScopedReleaser clearFault([]
        {
            WindowsGraphicsCapture::createItemFailureForTests_ = {};
            WindowsGraphicsCapture::startSessionFailureForTests_ = {};
        });
        Window::Data1 data{};
        data.hWnd = GetDesktopWindow(); data.isDesktop = TRUE;
        data.hMonitor = MonitorFromWindow(handle, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(data.hMonitor, &monitor)) winrt::throw_last_error();
        data.windowRect = data.clientRect = monitor.rcMonitor;
        Window window(92, data);
        WindowTexture texture(&window);
        texture.SetCaptureMode(CaptureMode::Auto);
        const auto capture = texture.GetWindowsGraphicsCapture();
        if (!capture) throw std::runtime_error("The monitor recovery object was discarded.");
        const auto& manager = WindowManager::GetWindowsGraphicsCaptureManager();
        Check(texture.GetCaptureModeInternal() == CaptureMode::BitBlt, "Auto retains a usable compatibility backend while WGC creation is denied");
        Check(capture->GetStatus().lastError == E_ACCESSDENIED, "access denial remains distinguishable from unsupported or invalid targets");
        for (int i = 0; i < 100; ++i)
        {
            texture.KeepCaptureAlive();
            manager->UpdateFromCaptureThread();
        }
        Check(capture->GetStatus().itemAttempts == 1, "per-frame capture requests do not bypass initial retry backoff");
        const uint64_t delays[] = {2000, 4000, 8000, 16000, 30000, 30000};
        for (const auto delay : delays)
        {
            capture->retryAt_ = 0;
            texture.KeepCaptureAlive();
            manager->UpdateFromCaptureThread();
            auto status = capture->GetStatus();
            Check(status.retryAfterMilliseconds <= delay && status.retryAfterMilliseconds + 100 >= delay,
                "repeated failures exponentially back off and cap at thirty seconds");
        }
        const auto attempts = capture->GetStatus().itemAttempts;
        capture->retryAt_ = 0;
        capture->lastRequestAt_ = GetTickCount64() - 2000;
        manager->UpdateFromCaptureThread();
        Check(capture->GetStatus().itemAttempts == attempts, "unrequested/offscreen sources do not retry indefinitely");
        WindowsGraphicsCapture::createItemFailureForTests_ = {};
        WindowsGraphicsCapture::startSessionFailureForTests_ = [] { return HRESULT_FROM_WIN32(ERROR_NOT_READY); };
        texture.KeepCaptureAlive();
        manager->UpdateFromCaptureThread();
        Check(capture->GetStatus().failureStage == static_cast<int>(WgcFailureStage::StartSession),
            "session-start failure has a separate diagnostic stage");
        Check(!capture->IsAvailable() && !capture->pool_ && !capture->session_,
            "failed session starts release partial resources and retain fallback");
        WindowsGraphicsCapture::startSessionFailureForTests_ = {};
        capture->retryAt_ = 0;
        texture.KeepCaptureAlive();
        manager->UpdateFromCaptureThread();
        const auto deadline = GetTickCount64() + 5000;
        while (!capture->IsAvailable() && GetTickCount64() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Check(capture->IsStarted() && capture->IsAvailable(), "a permitted monitor recovers to a live WGC session without recreating the AR desktop");
        Check(texture.GetCaptureModeInternal() == CaptureMode::WindowsGraphicsCapture, "Auto returns from BitBlt to GPU capture after a real source frame");
        Check(capture->GetStatus().recoveries == 1 && capture->GetStatus().consecutiveFailures == 0,
            "a real frame resets failure backoff and records recovery");
        const auto sessions = capture->GetStatus().sessionAttempts;
        for (int i = 0; i < 100; ++i) { texture.KeepCaptureAlive(); manager->UpdateFromCaptureThread(); }
        Check(capture->GetStatus().sessionAttempts == sessions, "repeated requests do not start duplicate sessions");
        Check(std::count(manager->activeInstances_.begin(), manager->activeInstances_.end(), capture) == 1,
            "a recovering capture has exactly one active-manager entry");
        texture.SetCaptureMode(CaptureMode::BitBlt);
        capture->lastRequestAt_ = GetTickCount64() - 2000;
        manager->UpdateFromCaptureThread();
        texture.KeepCaptureAlive();
        manager->UpdateFromCaptureThread();
        Check(!capture->IsStarted() && texture.GetCaptureModeInternal() == CaptureMode::BitBlt,
            "explicit BitBlt is respected and stops the unneeded GPU session");
        Check(capture->GetStatus().sessionAttempts == sessions, "explicit compatibility mode does not silently restart WGC");
        printf("MONITOR_RECOVERY hresult, backoff, idle suppression, session failure and actual monitor recovery checked\n");
    }

    static void ClosedItemRecovery()
    {
        HWND handle = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC",
            L"Owned WGC closed-item regression", WS_OVERLAPPEDWINDOW, 80, 80, 200, 160,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!handle) winrt::throw_last_error();
        ScopedReleaser close([&] { if (IsWindow(handle)) DestroyWindow(handle); });
        ShowWindow(handle, SW_SHOWNOACTIVATE);
        UpdateWindow(handle);
        const auto capture = std::make_shared<WindowsGraphicsCapture>(handle);
        capture->Start();
        const auto readyDeadline = GetTickCount64() + 5000;
        while (!capture->hasReceivedFrame_ && GetTickCount64() < readyDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Check(capture->hasReceivedFrame_, "closed-item regression begins with a live captured frame");
        DestroyWindow(handle);
        const auto closedDeadline = GetTickCount64() + 5000;
        while (!capture->itemClosed_ && GetTickCount64() < closedDeadline)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(capture->itemClosed_ && !capture->IsAvailable(), "the real capture-item Closed event withdraws WGC availability");
        Check(capture->ShouldRestart(), "a closed item requests bounded recovery without relying on static-frame silence");
        capture->Restart();
        const auto status = capture->GetStatus();
        Check(!capture->IsStarted() && status.lastError < 0 && status.retryAfterMilliseconds > 0,
            "closed-session recovery releases resources, preserves failure detail and observes backoff");
        Check(!capture->pool_ && !capture->session_, "closing the source does not retain a stale capture session");
        capture->Stop();
    }

    static void Bounds(HWND handle, int x, int y, bool virtualizedOrigin)
    {
        if (!SetWindowPos(handle, nullptr, x, y, 400, 240, SWP_NOACTIVATE | SWP_NOZORDER))
            winrt::throw_last_error();
        UpdateWindow(handle);
        Window::Data1 data{};
        data.hWnd = handle;
        if (!GetWindowRect(handle, &data.windowRect)) winrt::throw_last_error();
        if (!GetClientRect(handle, &data.clientRect)) winrt::throw_last_error();
        RECT visible{};
        winrt::check_hresult(DwmGetWindowAttribute(handle, DWMWA_EXTENDED_FRAME_BOUNDS,
            &visible, sizeof(visible)));
        if (virtualizedOrigin)
        {
            data.windowRect.left += 40;
            data.windowRect.right += 40;
            data.windowRect.top -= 20;
            data.windowRect.bottom -= 20;
        }
        Window window(1, data);
        WindowTexture texture(&window);
        Check(texture.CaptureByWindowsGraphicsCapture(), "WGC capture metadata is available");
        Check(static_cast<LONG>(window.GetX() + texture.GetOffsetX()) == visible.left,
            "window X plus capture offset is the physical visible-frame X");
        Check(static_cast<LONG>(window.GetY() + texture.GetOffsetY()) == visible.top,
            "window Y plus capture offset is the physical visible-frame Y");
        Check(texture.GetWidth() == static_cast<UINT>(visible.right - visible.left),
            "capture width matches the visible frame rather than invisible resize borders");
        Check(texture.GetHeight() == static_cast<UINT>(visible.bottom - visible.top),
            "capture height matches the visible frame");
        printf("BOUNDS raw=(%ld,%ld) visible=(%ld,%ld) offset=(%ld,%ld) size=(%u,%u)\n",
            data.windowRect.left, data.windowRect.top, visible.left, visible.top,
            static_cast<LONG>(texture.GetOffsetX()), static_cast<LONG>(texture.GetOffsetY()),
            texture.GetWidth(), texture.GetHeight());
    }

    static void DesktopBounds()
    {
        auto monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) winrt::throw_last_error();
        Window::Data1 data{};
        data.isDesktop = TRUE;
        data.hWnd = GetDesktopWindow();
        data.hMonitor = monitor;
        data.windowRect = info.rcMonitor;
        Window window(2, data);
        WindowTexture texture(&window);
        Check(texture.CaptureByWindowsGraphicsCapture(), "desktop capture metadata is available");
        Check(texture.GetOffsetX() == 0 && texture.GetOffsetY() == 0,
            "desktop capture retains its monitor origin without window-border correction");
        Check(!texture.GetWindowsGraphicsCapture()->IsStarted(), "desktop metadata test does not start screen capture");
    }

    static int Run()
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!previousDpi) winrt::throw_last_error();
        Debug::SetMode(Debug::Mode::UnityLog);
        Debug::SetErrorFunc(NativeError);
        PrepareDevice();
        HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC",
            L"uWindowCapture regression", WS_OVERLAPPEDWINDOW, 40, 40, 400, 240,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) winrt::throw_last_error();
        ShowWindow(window, SW_SHOWNOACTIVATE);
        UpdateWindow(window);
        try
        {
            SessionSettings(window);
            FailedMonitorCaptureRemainsRecoverable();
            MonitorRetryAndRecovery(window);
            ClosedItemRecovery();
            RequestDrivenThread();
            Bounds(window, 40, 40, false);
            Bounds(window, -40, 80, false);
            Bounds(window, 80, 80, true);
            DesktopBounds();
            LifetimeDuringUpload(window);
            LifetimeStressAndCost();
            RenderReadback(window);
            MipReadbackAndSwitch(window);
        }
        catch (...)
        {
            DestroyWindow(window);
            WindowManager::Destroy();
            SetThreadDpiAwarenessContext(previousDpi);
            throw;
        }
        DestroyWindow(window);
        WindowManager::Destroy();
        SetThreadDpiAwarenessContext(previousDpi);
        printf("NATIVE_CURSOR_TESTS checks=%d failures=%d\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
};

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--worker-cadence") == 0)
    {
        std::vector<double> gaps;
        auto started = std::chrono::steady_clock::now();
        auto previous = started;
        ThreadLoop loop(L"Capture cadence measurement");
        loop.Start([&]
        {
            auto now = std::chrono::steady_clock::now();
            gaps.push_back(std::chrono::duration<double, std::milli>(now - previous).count());
            previous = now;
        }, std::chrono::microseconds(100));
        std::this_thread::sleep_for(std::chrono::seconds(4));
        loop.Stop();
        auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::sort(gaps.begin(), gaps.end());
        printf("WORKER_CADENCE requested_us=100 ticks=%zu seconds=%.3f rate_hz=%.3f p50_ms=%.3f p95_ms=%.3f max_ms=%.3f\n",
            gaps.size(), seconds, gaps.size() / seconds, gaps[gaps.size() / 2], gaps[gaps.size() * 95 / 100], gaps.back());
        return 0;
    }
    try { return CursorRegressionTests::Run(); }
    catch (const winrt::hresult_error& error)
    {
        fprintf(stderr, "WinRT test error: 0x%08x\n", static_cast<unsigned>(error.code().value));
        return 2;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "Test error: %s\n", error.what());
        return 2;
    }
}
