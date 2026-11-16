#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <winrt/base.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>
#include "Debug.h"
#include "IUnityGraphicsD3D11.h"
#include "Message.h"
#include "Unity.h"
#include "Util.h"
#include "WindowManager.h"
#include "WindowTexture.h"

extern IUnityInterfaces* g_unity;

namespace
{
    using Clock = std::chrono::steady_clock;
    using Microsoft::WRL::ComPtr;

    struct Fixture
    {
        HWND handle = nullptr;
        std::atomic<unsigned> frames{0};
    };

    LRESULT CALLBACK FixtureProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
                reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto fixture = reinterpret_cast<Fixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_PAINT && fixture)
        {
            PAINTSTRUCT paint{};
            auto dc = BeginPaint(window, &paint);
            auto frame = ++fixture->frames;
            auto brush = CreateSolidBrush(RGB(frame & 255, (frame >> 8) & 255, (frame >> 16) & 255));
            FillRect(dc, &paint.rcPaint, brush);
            DeleteObject(brush);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HANDLE FrameTimer(int milliseconds)
    {
        auto timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) winrt::throw_last_error();
        LARGE_INTEGER due{};
        due.QuadPart = -10000;
        if (!SetWaitableTimer(timer, &due, milliseconds, nullptr, nullptr, FALSE))
        {
            CloseHandle(timer);
            winrt::throw_last_error();
        }
        return timer;
    }

    double CpuSeconds()
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) winrt::throw_last_error();
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        return (k.QuadPart + u.QuadPart) * 1e-7;
    }

    std::vector<MONITORINFOEXW> Monitors()
    {
        std::vector<MONITORINFOEXW> monitors;
        if (!EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM state) -> BOOL
        {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(monitor, &info)) return FALSE;
            reinterpret_cast<std::vector<MONITORINFOEXW>*>(state)->push_back(info);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&monitors))) winrt::throw_last_error();
        return monitors;
    }

    void Inventory()
    {
        ComPtr<IDXGIFactory1> factory;
        winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter1> adapter;
            auto result = factory->EnumAdapters1(index, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            winrt::check_hresult(result);
            DXGI_ADAPTER_DESC1 desc{};
            winrt::check_hresult(adapter->GetDesc1(&desc));
            printf("CAPTURE_ADAPTER index=%u name=%ls luid=%08x:%08x\n",
                index, desc.Description, desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
            for (UINT outputIndex = 0;; ++outputIndex)
            {
                ComPtr<IDXGIOutput> output;
                result = adapter->EnumOutputs(outputIndex, &output);
                if (result == DXGI_ERROR_NOT_FOUND) break;
                winrt::check_hresult(result);
                DXGI_OUTPUT_DESC outputDesc{};
                winrt::check_hresult(output->GetDesc(&outputDesc));
                printf("CAPTURE_OUTPUT adapter=%u name=%ls monitor=%p attached=%d pixels=%ldx%ld\n",
                    index, outputDesc.DeviceName, outputDesc.Monitor, outputDesc.AttachedToDesktop,
                    outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left,
                    outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top);
            }
        }
        auto monitors = Monitors();
        for (size_t index = 0; index < monitors.size(); ++index)
        {
            const auto& monitor = monitors[index];
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (!EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) winrt::throw_last_error();
            printf("CAPTURE_MONITOR index=%zu name=%ls pixels=%lux%lu hz=%lu origin=%ld,%ld\n", index,
                monitor.szDevice, mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency,
                monitor.rcMonitor.left, monitor.rcMonitor.top);
        }
    }

    double Percentile(std::vector<double> values, unsigned percentile)
    {
        if (values.empty()) return 0;
        std::sort(values.begin(), values.end());
        return values[(values.size() - 1) * percentile / 100];
    }
}

struct CaptureThroughputTests
{
    inline static ComPtr<ID3D11Device> device;
    inline static std::atomic<int> errors{0};

    static ID3D11Device* UNITY_INTERFACE_API Device() { return device.Get(); }
    static IUnityInterface* UNITY_INTERFACE_API Interface(UnityInterfaceGUID)
    {
        static auto unityInterface = []
        {
            IUnityGraphicsD3D11 value{};
            value.GetDevice = Device;
            return value;
        }();
        return &unityInterface;
    }
    static void UNITY_INTERFACE_API Error(const char* text)
    {
        ++errors;
        fprintf(stderr, "NATIVE_ERROR %s\n", text);
    }

    struct Sample
    {
        std::shared_ptr<Window> window;
        ComPtr<ID3D11Texture2D> target;
        unsigned requests = 0, publications = 0;
        uint64_t version = 0;
        double nextRequest = 0, lastPublication = 0;
        std::vector<double> gaps;
        std::vector<unsigned> bins;
        std::vector<double> sourceAges;
        int64_t lastSourceTime = 0;
        unsigned uniqueSourceFrames = 0;
    };

    static int Run(int count, int width, int height, int seconds, int animatedCount, int adapterIndex, bool desktop,
        int monitorIndex, bool mipmaps, CaptureMode mode, int initialWgcFailures)
    {
        if (count < 1 || count > 16 || width < 64 || height < 64 || seconds < 3 || seconds > 120 ||
            animatedCount < 1 || animatedCount > count)
            throw std::invalid_argument("Expected 1..16 windows, dimensions >=64, duration 3..120 seconds.");
        if (desktop && count != 1) throw std::invalid_argument("Desktop capture requires one source fixture.");
        if (initialWgcFailures < 0 || initialWgcFailures > 3 ||
            (initialWgcFailures > 0 && (!desktop || mode != CaptureMode::Auto)))
            throw std::invalid_argument("Recovery injection requires one Auto desktop and 1..3 initial WGC failures.");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!previousDpi) winrt::throw_last_error();
        ScopedReleaser restoreDpi([&] { SetThreadDpiAwarenessContext(previousDpi); winrt::uninit_apartment(); });
        std::atomic<int> failuresRemaining{initialWgcFailures};
        if (initialWgcFailures > 0)
            WindowsGraphicsCapture::createItemFailureForTests_ = [&]
            {
                if (failuresRemaining > 0) { --failuresRemaining; return E_ACCESSDENIED; }
                return S_OK;
            };
        ScopedReleaser clearFault([] { WindowsGraphicsCapture::createItemFailureForTests_ = {}; });
        Inventory();
        RECT fixtureMonitor{};
        if (monitorIndex >= 0)
        {
            const auto monitors = Monitors();
            if (static_cast<size_t>(monitorIndex) >= monitors.size()) throw std::invalid_argument("Unknown monitor index.");
            fixtureMonitor = monitors[monitorIndex].rcMonitor;
        }
        std::vector<std::unique_ptr<Fixture>> fixtures;
        for (int i = 0; i < count; ++i) fixtures.push_back(std::make_unique<Fixture>());
        std::atomic<bool> stop{false};
        std::promise<void> ready;
        auto readyFuture = ready.get_future();
        std::exception_ptr fixtureFailure;
        std::thread painter([&]
        {
            HANDLE timer = nullptr;
            bool signaled = false;
            try
            {
                SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
                WNDCLASSW type{};
                type.lpfnWndProc = FixtureProc;
                type.hInstance = GetModuleHandleW(nullptr);
                type.lpszClassName = L"UwcThroughputFixture";
                if (!RegisterClassW(&type)) winrt::throw_last_error();
                timer = FrameTimer(16);
                for (int i = 0; i < count; ++i)
                {
                    auto& fixture = *fixtures[i];
                    fixture.handle = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, type.lpszClassName,
                        L"Owned capture throughput fixture", WS_OVERLAPPEDWINDOW,
                        fixtureMonitor.left + 50 + i * 16, fixtureMonitor.top + 50 + i * 16,
                        width, height, nullptr, nullptr, type.hInstance, &fixture);
                    if (!fixture.handle) winrt::throw_last_error();
                    ShowWindow(fixture.handle, SW_SHOWNOACTIVATE);
                    UpdateWindow(fixture.handle);
                }
                ready.set_value(); signaled = true;
                while (!stop)
                {
                    auto result = MsgWaitForMultipleObjects(1, &timer, FALSE, INFINITE, QS_ALLINPUT);
                    if (result == WAIT_FAILED) winrt::throw_last_error();
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    {
                        TranslateMessage(&message); DispatchMessageW(&message);
                    }
                    if (result == WAIT_OBJECT_0)
                        for (int i = 0; i < animatedCount; ++i)
                        {
                            const auto& fixture = fixtures[i];
                            InvalidateRect(fixture->handle, nullptr, FALSE);
                            UpdateWindow(fixture->handle);
                        }
                }
            }
            catch (...)
            {
                fixtureFailure = std::current_exception();
                if (!signaled) ready.set_exception(fixtureFailure);
            }
            for (const auto& fixture : fixtures) if (fixture->handle) DestroyWindow(fixture->handle);
            if (timer) CloseHandle(timer);
            UnregisterClassW(L"UwcThroughputFixture", GetModuleHandleW(nullptr));
        });
        ScopedReleaser stopPainter([&] { stop = true; if (painter.joinable()) painter.join(); });
        readyFuture.get();

        ComPtr<IDXGIFactory1> factory;
        winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> selected;
        winrt::check_hresult(factory->EnumAdapters1(adapterIndex, &selected));
        winrt::check_hresult(D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
        ComPtr<IDXGIDevice> dxgiDevice;
        winrt::check_hresult(device.As(&dxgiDevice));
        ComPtr<IDXGIAdapter> adapter;
        winrt::check_hresult(dxgiDevice->GetAdapter(&adapter));
        DXGI_ADAPTER_DESC adapterDesc{};
        winrt::check_hresult(adapter->GetDesc(&adapterDesc));
        printf("CAPTURE_GPU name=%ls dedicated_mb=%.3f\n", adapterDesc.Description,
            adapterDesc.DedicatedVideoMemory / 1048576.0);
        IUnityInterfaces interfaces{};
        interfaces.GetInterface = Interface;
        g_unity = &interfaces;
        MessageManager::Create();
        WindowManager::Create();
        ScopedReleaser cleanup([&]
        {
            WindowManager::Get().Finalize();
            WindowManager::Destroy(); MessageManager::Destroy();
            g_unity = nullptr; device.Reset();
        });
        Debug::SetMode(Debug::Mode::UnityLog); Debug::SetErrorFunc(Error);
        auto& manager = WindowManager::Get();
        manager.windowsGraphicsCaptureManager_ = std::make_unique<WindowsGraphicsCaptureManager>();
        manager.uploadManager_ = std::make_unique<UploadManager>();
        manager.uploadManager_->initThread_.join();
        if (!manager.uploadManager_->GetDevice()) throw std::runtime_error("Upload device unavailable.");

        std::vector<Sample> samples(count);
        for (int i = 0; i < count; ++i)
        {
            Window::Data1 data{};
            data.hWnd = fixtures[i]->handle;
            GetWindowRect(data.hWnd, &data.windowRect); GetClientRect(data.hWnd, &data.clientRect);
            data.hMonitor = MonitorFromWindow(data.hWnd, MONITOR_DEFAULTTONEAREST);
            if (desktop)
            {
                data.isDesktop = TRUE;
                data.hWnd = GetDesktopWindow();
                MONITORINFO monitor{};
                monitor.cbSize = sizeof(monitor);
                if (!GetMonitorInfoW(data.hMonitor, &monitor)) winrt::throw_last_error();
                data.windowRect = data.clientRect = monitor.rcMonitor;
            }
            auto& sample = samples[i];
            sample.window = manager.FindOrAddWindow(data);
            sample.window->InitTexture();
            sample.window->SetCaptureMode(mode);
            sample.window->SetCursorDraw(false);
            if (!sample.window->windowTexture_->Capture()) throw std::runtime_error("Initial capture metadata unavailable.");
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = sample.window->GetTextureWidth(); desc.Height = sample.window->GetTextureHeight();
            desc.MipLevels = mipmaps ? 0 : 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS; desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags = mipmaps ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
            winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, &sample.target));
            sample.window->SetWindowTexture(sample.target.Get(), desc.Width, desc.Height, true);
            printf("CAPTURE_SOURCE index=%d desktop=%d pixels=%ux%u fixture=%dx%d mipmaps=%d monitor=%p\n",
                i, desktop, desc.Width, desc.Height, width, height, mipmaps, data.hMonitor);
            sample.bins.resize(seconds);
        }
        manager.cursor_ = std::make_unique<Cursor>();
        manager.captureManager_ = std::make_unique<CaptureManager>();
        auto timer = FrameTimer(8);
        ScopedReleaser closeTimer([&] { CloseHandle(timer); });
        auto start = Clock::now();
        const double warmup = initialWgcFailures > 0 ? (1 << initialWgcFailures) + 2 : 2;
        double previousTime = 0, cpuStart = 0;
        std::vector<unsigned> sourceStarts(count);
        bool measuring = false;
        bool observedFallback = false, observedWgc = false;
        std::vector<double> renderTimes;
        std::vector<WindowCaptureStatistics> starts(count);
        while (true)
        {
            if (WaitForSingleObject(timer, 1000) != WAIT_OBJECT_0) throw std::runtime_error("Render timer failed.");
            double time = std::chrono::duration<double>(Clock::now() - start).count();
            if (time >= warmup + seconds) break;
            if (!measuring && time >= warmup)
            {
                measuring = true; cpuStart = CpuSeconds();
                for (int i = 0; i < count; ++i)
                {
                    sourceStarts[i] = fixtures[i]->frames;
                    starts[i] = samples[i].window->GetCaptureStatistics();
                }
            }
            manager.Update(static_cast<float>(time - previousTime)); previousTime = time;
            for (int i = 0; i < count; ++i)
            {
                auto& sample = samples[i];
                if (time >= sample.nextRequest)
                {
                    manager.captureManager_->RequestCapture(sample.window->GetId(),
                        i == 0 ? CapturePriority::High : CapturePriority::Low);
                    if (measuring) ++sample.requests;
                    double period = 1.0 / (i == 0 ? 30 : 15);
                    do { sample.nextRequest += period; } while (sample.nextRequest <= time);
                }
            }
            const auto beforeRender = Clock::now();
            manager.Render();
            if (initialWgcFailures > 0)
            {
                const auto backend = samples[0].window->GetRenderedCaptureMode(samples[0].target.Get());
                observedFallback = observedFallback || backend == CaptureMode::BitBlt;
                observedWgc = observedWgc || backend == CaptureMode::WindowsGraphicsCapture;
            }
            if (measuring) renderTimes.push_back(std::chrono::duration<double, std::milli>(Clock::now() - beforeRender).count());
            LARGE_INTEGER clock{}, frequency{};
            QueryPerformanceCounter(&clock); QueryPerformanceFrequency(&frequency);
            const double qpcTime = clock.QuadPart * (10000000.0 / frequency.QuadPart);
            for (auto& sample : samples)
            {
                auto& texture = *sample.window->windowTexture_;
                std::lock_guard<std::mutex> lock(texture.renderedTextureMutex_);
                auto version = texture.renderedVersion_;
                if (version == sample.version) continue;
                sample.version = version;
                if (!measuring) continue;
                ++sample.publications;
                const int64_t sourceTime = texture.publishedSourceTime_;
                if (sourceTime != sample.lastSourceTime) ++sample.uniqueSourceFrames;
                sample.lastSourceTime = sourceTime;
                if (sourceTime > 0) sample.sourceAges.push_back((qpcTime - sourceTime) / 10000);
                int bin = static_cast<int>(time - warmup);
                if (bin < seconds) ++sample.bins[bin];
                if (sample.lastPublication > 0) sample.gaps.push_back((time - sample.lastPublication) * 1000);
                sample.lastPublication = time;
            }
            MessageManager::Get().ClearAll();
        }
        double elapsed = std::chrono::duration<double>(Clock::now() - start).count() - warmup;
        double cpu = CpuSeconds() - cpuStart;
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
            winrt::throw_last_error();
        printf("CAPTURE_RESOURCE windows=%d width=%d height=%d seconds=%.3f cpu_core_percent=%.3f working_mb=%.3f private_mb=%.3f peak_working_mb=%.3f\n",
            count, width, height, elapsed, cpu / elapsed * 100,
            memory.WorkingSetSize / 1048576.0, memory.PrivateUsage / 1048576.0, memory.PeakWorkingSetSize / 1048576.0);
        printf("CAPTURE_RENDER_CPU p50_ms=%.3f p95_ms=%.3f max_ms=%.3f\n", Percentile(renderTimes, 50),
            Percentile(renderTimes, 95), Percentile(renderTimes, 100));
        bool passed = errors == 0;
        if (initialWgcFailures > 0)
        {
            const auto status = samples[0].window->GetWgcStatus();
            const bool recovered = observedFallback && observedWgc && status.available &&
                status.recoveries == 1 && status.itemAttempts == static_cast<uint64_t>(initialWgcFailures + 1) &&
                samples[0].window->windowTexture_->buffer_.Empty() && !samples[0].window->windowTexture_->bitmap_;
            printf("CAPTURE_RECOVERY fallback_seen=%d wgc_seen=%d attempts=%llu recoveries=%llu recovered=%d\n",
                observedFallback, observedWgc, status.itemAttempts, status.recoveries, recovered);
            passed = passed && recovered;
        }
        for (int i = 0; i < count; ++i)
        {
            auto& sample = samples[i];
            if (sample.gaps.empty())
            {
                if (i < animatedCount) throw std::runtime_error("Changing source did not publish capture frames.");
                printf("CAPTURE_STATIC index=%d publications=%u reused_last_frame=1\n", i, sample.publications);
                continue;
            }
            std::sort(sample.gaps.begin(), sample.gaps.end());
            double target = i == 0 ? 30 : 15;
            double fps = sample.publications / elapsed;
            double sourceFps = (fixtures[i]->frames.load() - sourceStarts[i]) / elapsed;
            auto minimum = *std::min_element(sample.bins.begin(), sample.bins.end());
            printf("CAPTURE_WINDOW index=%d target=%.0f source_fps=%.3f requests=%u published=%u fps=%.3f min_bin=%u p50_ms=%.3f p95_ms=%.3f max_ms=%.3f backend=%d\n",
                i, target, sourceFps, sample.requests, sample.publications, fps, minimum,
                sample.gaps[sample.gaps.size() / 2], sample.gaps[sample.gaps.size() * 95 / 100], sample.gaps.back(),
                static_cast<int>(sample.window->GetRenderedCaptureMode(sample.target.Get())));
            auto wgc = sample.window->windowTexture_->GetWindowsGraphicsCapture();
            auto statistics = sample.window->GetCaptureStatistics();
            if (statistics.requests - starts[i].requests != sample.requests)
                throw std::runtime_error("Request statistics must count caller submissions, not queue processing.");
            printf("CAPTURE_CADENCE index=%d source_fps=%.3f upload_fps=%.3f publication_fps=%.3f unique_fps=%.3f source_age_p50_ms=%.3f source_age_p95_ms=%.3f\n",
                i, (statistics.sourceFrames - starts[i].sourceFrames) / elapsed,
                (statistics.uploads - starts[i].uploads) / elapsed,
                (statistics.publications - starts[i].publications) / elapsed, sample.uniqueSourceFrames / elapsed,
                Percentile(sample.sourceAges, 50), Percentile(sample.sourceAges, 95));
            std::lock_guard<std::mutex> shared(sample.window->windowTexture_->sharedTextureMutex_);
            printf("CAPTURE_STAGE index=%d arrived_total=%llu drained_total=%llu empty_reads=%llu uploaded_total=%llu rendered_version=%llu\n",
                i, wgc->arrivedFrames_.load(), wgc->drainedFrames_.load(), wgc->emptyReads_.load(),
                sample.window->windowTexture_->sharedVersion_, sample.version);
            if (i < animatedCount)
                passed = passed && sourceFps >= 35 && fps >= target * .97 && minimum >= target * .8 && sample.gaps.back() <= 200;
        }
        manager.captureManager_.reset();
        manager.uploadManager_->StopUploadThread();
        manager.cursor_->StopCapture();
        stop = true;
        painter.join();
        if (fixtureFailure) std::rethrow_exception(fixtureFailure);
        printf("CAPTURE_THROUGHPUT passed=%d errors=%d\n", passed, errors.load());
        return passed ? 0 : 1;
    }
};

int main(int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::strcmp(argv[1], "--inventory") == 0) { Inventory(); return 0; }
        return CaptureThroughputTests::Run(argc > 1 ? std::stoi(argv[1]) : 8,
            argc > 2 ? std::stoi(argv[2]) : 1280, argc > 3 ? std::stoi(argv[3]) : 720,
            argc > 4 ? std::stoi(argv[4]) : 8,
            argc > 5 ? std::stoi(argv[5]) : (argc > 1 ? std::stoi(argv[1]) : 8),
            argc > 6 ? std::stoi(argv[6]) : 0, argc > 7 && std::stoi(argv[7]) != 0,
            argc > 8 ? std::stoi(argv[8]) : -1, argc <= 9 || std::stoi(argv[9]) != 0,
            argc > 10 ? static_cast<CaptureMode>(std::stoi(argv[10])) : CaptureMode::Auto,
            argc > 11 ? std::stoi(argv[11]) : 0);
    }
    catch (const winrt::hresult_error& error)
    {
        fprintf(stderr, "WinRT capture test error: 0x%08x\n", static_cast<unsigned>(error.code().value));
        return 2;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "Capture test error: %s\n", error.what());
        return 2;
    }
}
