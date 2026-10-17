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
            Check(correct, "GPU readback contains the uploaded pixels after resize/replacement");
        }
        capture.SetUnityTexturePtr(nullptr);
        Check(!capture.Render(), "cleared Unity target is not rendered");
        printf("TEXTURE_GPU_READBACK replacements=32 checked\n");
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
            Check(capture->IsStarted(), "capture recovery restarts the session");
            Check(!capture->session_.IsCursorCaptureEnabled(), "capture recovery retains cursor exclusion");
        }
        capture->EnableCursorCapture(true);
        capture->Restart();
        Check(capture->session_.IsCursorCaptureEnabled(), "restart also preserves an enabled cursor");
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
            Bounds(window, 40, 40, false);
            Bounds(window, -40, 80, false);
            Bounds(window, 80, 80, true);
            DesktopBounds();
            LifetimeDuringUpload(window);
            LifetimeStressAndCost();
            RenderReadback(window);
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

int main()
{
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
