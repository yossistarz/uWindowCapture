#include <windows.h>
#include <dwmapi.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <cstdio>
#include <stdexcept>
#include "WindowsGraphicsCapture.h"
#include "WindowManager.h"
#include "WindowTexture.h"
#include "Debug.h"

struct CursorRegressionTests
{
    inline static int checks = 0;
    inline static int failures = 0;

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
        auto dxgi = device.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        auto manager = std::make_unique<WindowsGraphicsCaptureManager>();
        manager->deviceWinRt_ = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        WindowManager::Create();
        WindowManager::Get().windowsGraphicsCaptureManager_ = std::move(manager);
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
