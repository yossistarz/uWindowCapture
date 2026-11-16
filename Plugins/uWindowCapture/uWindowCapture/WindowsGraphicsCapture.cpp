#include "WindowsGraphicsCapture.h"
#include "WindowManager.h"
#include "UploadManager.h"
#include "Debug.h"
#include "Util.h"
#include "Window.h"
#include <inspectable.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.foundation.h>
#include <Windows.h>
#include <algorithm>

#pragma comment(lib, "windowsapp")

using namespace winrt;
using namespace winrt::Windows;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace ::Windows::Graphics::DirectX::Direct3D11;


namespace
{

bool CallWinRtApiWithExceptionCheck(const std::function<void()> &func, const std::string& name) noexcept
{
    try
    {
        func();
    }
    catch (const winrt::hresult_error& e)
    {
        const int code = e.code();
        char buf[256];
        sprintf_s(buf, 256, "0x%x", code);
        const auto msg = winrt::to_string(e.message());
        Debug::Error(name, " threw an WinRT exception: ", buf, " ", msg);
        return false;
    }
    catch (const std::exception& e)
    {
        Debug::Error(name, " threw an std exception: ", e.what());
        return false;
    }
    catch (...)
    {
        Debug::Error(name, " threw an unknown exception.");
        return false;
    }

    return true;
}


}


// ---

void WindowsGraphicsCapture::Result::Close() noexcept
{
    if (frame)
        CallWinRtApiWithExceptionCheck([&] { frame.Close(); }, "WindowsGraphicsCapture::Result::Close()");
    frame = nullptr;
}


bool WindowsGraphicsCapture::IsSupported()
{
    using ApiInfo = winrt::Windows::Foundation::Metadata::ApiInformation;

    static const bool available = []
    {
        bool supported = false;
        CallWinRtApiWithExceptionCheck([&]
        {
            supported = ApiInfo::IsApiContractPresent(L"Windows.Foundation.UniversalApiContract", 8) &&
                GraphicsCaptureSession::IsSupported();
        }, "WindowsGraphicsCapture::IsSupported()");
        return supported;
    }();
    return available;
}


bool WindowsGraphicsCapture::IsCursorCaptureEnabledApiSupported()
{
    using ApiInfo = winrt::Windows::Foundation::Metadata::ApiInformation;

    if (!IsSupported()) return false;

    static bool isChecked = false;
    static bool isEnabled = false;
    if (isChecked) return isEnabled;
    isChecked = true;

    if (!CallWinRtApiWithExceptionCheck([&]
    {
        isEnabled = ApiInfo::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession",
            L"IsCursorCaptureEnabled");
    }, "WindowsGraphicsCapture::IsCursorCaptureEnabledApiAvailable()"))
    {
        return false;
    }

    return isEnabled;
}


WindowsGraphicsCapture::WindowsGraphicsCapture(HWND hWnd)
    : hWnd_(hWnd)
    , hMonitor_(NULL)
{
    CreateItem();
}


WindowsGraphicsCapture::WindowsGraphicsCapture(HMONITOR hMonitor)
    : hMonitor_(hMonitor)
    , hWnd_(NULL)
{
    CreateItem();
}


bool WindowsGraphicsCapture::CreateItem()
{
    ++itemAttempts_;
    if (!IsSupported())
    {
        RecordFailure(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), WgcFailureStage::CreateItem);
        return false;
    }

    std::scoped_lock lock(itemMutex_);
    itemReady_ = false;
    try
    {
        RevokeItemClosed();
        item_ = nullptr;
#ifdef UWC_NATIVE_TESTS
        if (createItemFailureForTests_) check_hresult(createItemFailureForTests_());
#endif
        const auto factory = get_activation_factory<GraphicsCaptureItem>();
        const auto interop = factory.as<IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem replacement = nullptr;
        if (hWnd_)
        {
            check_hresult(interop->CreateForWindow(
                hWnd_,
                guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), 
                reinterpret_cast<void**>(put_abi(replacement))));
        }
        else
        {
            check_hresult(interop->CreateForMonitor(
                hMonitor_,
                guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), 
                reinterpret_cast<void**>(put_abi(replacement))));
        }
        if (!replacement) throw_hresult(E_UNEXPECTED);
        const auto size = replacement.Size();
        if (size.Width <= 0 || size.Height <= 0) throw_hresult(HRESULT_FROM_WIN32(ERROR_NOT_READY));
        item_ = std::move(replacement);
        size_ = size;
        itemClosed_ = false;
        itemReady_ = true;
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        RecordFailure(error.code(), WgcFailureStage::CreateItem);
        return false;
    }
}


bool WindowsGraphicsCapture::CreatePoolAndSession()
{
    std::scoped_lock lock(itemMutex_);

    if (!item_) throw_hresult(E_UNEXPECTED);

    size_ = item_.Size();
    if (size_.Width <= 0 || size_.Height <= 0) throw_hresult(HRESULT_FROM_WIN32(ERROR_NOT_READY));

    const auto& manager = WindowManager::GetWindowsGraphicsCaptureManager();
    if (!manager) throw_hresult(E_PENDING);

    auto& device = manager->GetDevice();

    ++sessionAttempts_;
#ifdef UWC_NATIVE_TESTS
    if (startSessionFailureForTests_) check_hresult(startSessionFailureForTests_());
#endif
    {
        std::scoped_lock lock(sessionAndPoolMutex_);

        hasReceivedFrame_ = false;
        pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
            device,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            size_);
        const auto weak = weak_from_this();
        const auto generation = ++poolGeneration_;
        frameArrivedToken_ = pool_.FrameArrived([weak, generation](const auto& sender, const auto&)
        {
            if (const auto capture = weak.lock())
                capture->OnFrameArrived(sender, generation);
        });
        if (!itemClosedSubscribed_)
        {
            itemClosedToken_ = item_.Closed([weak](const auto&, const auto&)
            {
                if (const auto capture = weak.lock()) capture->itemClosed_ = true;
            });
            itemClosedSubscribed_ = true;
        }
        session_ = pool_.CreateCaptureSession(item_);
        // Every new session starts with Windows' default, not our cached setting.
        if (IsCursorCaptureEnabledApiSupported())
        {
            session_.IsCursorCaptureEnabled(isCursorCaptureEnabled_.load());
        }
        session_.StartCapture();
    }
    return true;
}


void WindowsGraphicsCapture::DestroyPoolAndSession()
{
    std::scoped_lock lock(sessionAndPoolMutex_);
    {
        std::scoped_lock frameLock(frameMutex_);
        ++poolGeneration_;
    }

    if (pool_)
    {
        pool_.FrameArrived(frameArrivedToken_);
        ReleaseLatestResult();
        CallWinRtApiWithExceptionCheck(
            [&] { pool_.Close(); }, 
            "WindowsGraphicsCapture::DestroyPoolAndSession() - Pool");
    }

    if (session_)
    {
        CallWinRtApiWithExceptionCheck(
            [&] { session_.Close(); }, 
            "WindowsGraphicsCapture::DestroyPoolAndSession() - Session");
    }

    pool_ = nullptr;
    session_ = nullptr;
    ReleaseLatestResult();
}

void WindowsGraphicsCapture::OnFrameArrived(const Direct3D11CaptureFramePool& sender, uint64_t generation)
{
    if (generation != poolGeneration_) return;
    ++arrivedFrames_;
    bool received = false;
    bool recovered = false;
    try
    {
        std::scoped_lock lock(frameMutex_);
        if (generation != poolGeneration_) return;
        while (auto next = sender.TryGetNextFrame())
        {
#ifdef UWC_NATIVE_TESTS
            ++drainedFrames_;
#endif
            if (frame_) frame_.Close();
            frame_ = std::move(next);
            received = true;
        }
        if (received)
        {
            hasReceivedFrame_ = true;
            consecutiveFailures_ = 0;
            retryAt_ = 0;
            if (recovering_.exchange(false))
            {
                ++recoveries_;
                recovered = true;
            }
        }
    }
    catch (const winrt::hresult_error& error)
    {
        if (generation == poolGeneration_)
        {
            if (!isRestartRequested_.exchange(true))
                RecordFailure(error.code(), WgcFailureStage::ReadFrame);
        }
        return;
    }
    if (recovered) NotifyStatusChanged();
    if (frameRequested_ && frameAvailableCallback_) frameAvailableCallback_();
}


WindowsGraphicsCapture::~WindowsGraphicsCapture()
{
}


void WindowsGraphicsCapture::RequestStart()
{
    lastRequestAt_ = GetTickCount64();
    isStartRequested_ = true;
}


void WindowsGraphicsCapture::Start()
{
    isStartRequested_ = false;

    if (isStarted_) return;

    if (GetTickCount64() < retryAt_) return;
    try
    {
        if ((!itemReady_ || recovering_ || itemClosed_) && !CreateItem()) return;
        CreatePoolAndSession();
        isStarted_ = true;
        lastRequestAt_ = GetTickCount64();
        sessionStartedAt_ = GetTickCount64();
    }
    catch (const winrt::hresult_error& error)
    {
        DestroyPoolAndSession();
        RecordFailure(error.code(), WgcFailureStage::StartSession);
    }
}


void WindowsGraphicsCapture::Stop()
{
    isStarted_ = false;

    DestroyPoolAndSession();
    {
        std::scoped_lock lock(itemMutex_);
        RevokeItemClosed();
    }
    const bool waiting = frameRequested_.exchange(false);
    if (waiting && frameAvailableCallback_) frameAvailableCallback_();
}


bool WindowsGraphicsCapture::ShouldStop() const
{
    return GetTickCount64() - lastRequestAt_ > 1000;
}


bool WindowsGraphicsCapture::ShouldRestart() const
{
    return itemClosed_ || isRestartRequested_ ||
        (isStarted_ && !hasReceivedFrame_ && GetTickCount64() - sessionStartedAt_ > 1000);
}

bool WindowsGraphicsCapture::CanStart() const
{
    const auto now = GetTickCount64();
    return isStartRequested_ && !isStarted_ && IsSupported() &&
        now - lastRequestAt_ <= 1000 && now >= retryAt_;
}

void WindowsGraphicsCapture::RecordFailure(HRESULT error, WgcFailureStage stage)
{
    lastError_ = error;
    failureStage_ = static_cast<int32_t>(stage);
    recovering_ = true;
    const int failures = ++consecutiveFailures_;
    const uint64_t delay = (std::min)(uint64_t{30'000}, uint64_t{1000} << (std::min)(failures - 1, 5));
    retryAt_ = GetTickCount64() + delay;
    char code[16]{};
    sprintf_s(code, "0x%08X", static_cast<unsigned>(error));
    Debug::Log("WARNING: WGC ", hMonitor_ ? "monitor=" : "window=",
        hMonitor_ ? reinterpret_cast<void*>(hMonitor_) : reinterpret_cast<void*>(hWnd_),
        " stage=", static_cast<int>(stage), " HRESULT=", code, " failures=", failures, " retryMs=", delay);
    NotifyStatusChanged();
}

void WindowsGraphicsCapture::RevokeItemClosed()
{
    if (item_ && itemClosedSubscribed_)
    {
        CallWinRtApiWithExceptionCheck([&] { item_.Closed(itemClosedToken_); },
            "WindowsGraphicsCapture::RevokeItemClosed()");
        itemClosedSubscribed_ = false;
    }
}

void WindowsGraphicsCapture::SetStatusChangedCallback(std::function<void()> callback)
{
    statusChangedCallback_ = std::move(callback);
    if (FAILED(lastError_.load())) NotifyStatusChanged();
}

void WindowsGraphicsCapture::NotifyStatusChanged()
{
    if (statusChangedCallback_) statusChangedCallback_();
}

WgcCaptureStatus WindowsGraphicsCapture::GetStatus() const
{
    WgcCaptureStatus result{};
    result.itemAttempts = itemAttempts_;
    result.sessionAttempts = sessionAttempts_;
    result.recoveries = recoveries_;
    const auto now = GetTickCount64();
    const auto retryAt = retryAt_.load();
    result.retryAfterMilliseconds = recovering_ && retryAt > now ? retryAt - now : 0;
    result.targetHandle = hMonitor_ ? reinterpret_cast<uintptr_t>(hMonitor_) : reinterpret_cast<uintptr_t>(hWnd_);
    result.lastError = lastError_;
    result.failureStage = failureStage_;
    result.consecutiveFailures = consecutiveFailures_;
    result.available = IsAvailable();
    result.started = isStarted_;
    result.supported = IsSupported();
    return result;
}

void WindowsGraphicsCapture::SetFrameAvailableCallback(std::function<void()> callback)
{
    frameAvailableCallback_ = std::move(callback);
}

void WindowsGraphicsCapture::Restart()
{
    isRestartRequested_ = false;
    const bool closed = itemClosed_.exchange(false);
    if (!recovering_)
        RecordFailure(closed ? RO_E_CLOSED : HRESULT_FROM_WIN32(ERROR_TIMEOUT),
            closed ? WgcFailureStage::ItemClosed : WgcFailureStage::FirstFrame);
    Stop();
}


bool WindowsGraphicsCapture::IsAvailable() const
{
    return itemReady_ && !recovering_ && !itemClosed_;
}


void WindowsGraphicsCapture::EnableCursorCapture(bool enabled)
{
    std::scoped_lock lock(sessionAndPoolMutex_);
    if (isCursorCaptureEnabled_ == enabled) return;

    if (!IsCursorCaptureEnabledApiSupported())
    {
        isCursorCaptureEnabled_ = enabled;
        Debug::Log("CursorCaptureEnabled API is not available.");
        return;
    }

    if (session_ && !CallWinRtApiWithExceptionCheck(
        [&] { session_.IsCursorCaptureEnabled(enabled); },
        "WindowsGraphicsCapture::EnableCursorCapture()"))
    {
        return;
    }
    isCursorCaptureEnabled_ = enabled;
}


WindowsGraphicsCapture::Result WindowsGraphicsCapture::TryGetLatestResult()
{
    UWC_SCOPE_TIMER(TryGetLatestResult)

    std::scoped_lock lock(sessionAndPoolMutex_);

    if (!pool_) return {};

    frameRequested_ = true;
    std::scoped_lock frameLock(frameMutex_);

    if (!frame_)
    {
#ifdef UWC_NATIVE_TESTS
        ++emptyReads_;
#endif
        return {};
    };

    auto selected = std::exchange(frame_, nullptr);
    const auto surface = selected.Surface();
    if (!surface) return {};

    frameRequested_ = false;

    auto access = surface.as<IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    const auto hr = access->GetInterface(guid_of<ID3D11Texture2D>(), texture.put_void());
    if (FAILED(hr)) return {};

    const auto size = selected.ContentSize();
    const bool hasSizeChanged = 
        (size_.Width != size.Width) || 
        (size_.Height != size.Height);

    const int64_t sourceTime = selected.SystemRelativeTime().count();
    return { texture.get(), size.Width, size.Height, hasSizeChanged,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>(texture.get()), std::move(selected), sourceTime };
}


void WindowsGraphicsCapture::ReleaseLatestResult()
{
    std::scoped_lock lock(frameMutex_);

    if (frame_)
    {
        CallWinRtApiWithExceptionCheck([&] { frame_.Close(); }, "WindowsGraphicsCapture::ReleaseLatestResult()");
        frame_ = nullptr;
    }
}


void WindowsGraphicsCapture::ChangePoolSize(int width, int height)
{
    std::scoped_lock lock(sessionAndPoolMutex_);

    if (!pool_) return;

    const auto& manager = WindowManager::GetWindowsGraphicsCaptureManager();
    if (!manager) return;

    auto &device = manager->GetDevice();
    ReleaseLatestResult();
    size_ = { width, height };

    pool_.Recreate(
        device, 
        DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,
        size_);
}


const wchar_t * WindowsGraphicsCapture::GetDisplayName() const
{
    std::scoped_lock lock(itemMutex_);

    static const wchar_t invalid[] = L"";
    if (!item_) return invalid;

    if (!CallWinRtApiWithExceptionCheck([&]
    {
        const auto displayName = item_.DisplayName();
    }, "WindowsGraphicsCapture::GetDisplayName()"))
    {
        return invalid;
    }

    return item_.DisplayName().c_str();
}


// ---


IDirect3DDevice & WindowsGraphicsCaptureManager::GetDevice()
{
    if (!deviceWinRt_)
    {
        const auto& uploader = WindowManager::GetUploadManager();
        if (!uploader || !uploader->IsReady()) throw_hresult(E_PENDING);

        if (auto device = uploader->GetDevice())
        {
            com_ptr<IDXGIDevice> dxgiDevice;
            check_hresult(device->QueryInterface<IDXGIDevice>(dxgiDevice.put()));
            com_ptr<::IInspectable> deviceWinRt;
            check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), deviceWinRt.put()));
            deviceWinRt_ = deviceWinRt.as<IDirect3DDevice>();
        }
        if (!deviceWinRt_) throw_hresult(E_UNEXPECTED);
    }

    return deviceWinRt_;
}


std::shared_ptr<WindowsGraphicsCapture> WindowsGraphicsCaptureManager::Create(HWND hWnd)
{
    auto instance = std::make_shared<WindowsGraphicsCapture>(hWnd);

    std::scoped_lock lock(allInstancesMutex_);
    allInstances_.push_back(instance);

    return instance;
}


std::shared_ptr<WindowsGraphicsCapture> WindowsGraphicsCaptureManager::Create(HMONITOR hMonitor)
{
    auto instance = std::make_shared<WindowsGraphicsCapture>(hMonitor);

    std::scoped_lock lock(allInstancesMutex_);
    allInstances_.push_back(instance);

    return instance;
}


void WindowsGraphicsCaptureManager::Destroy(const std::shared_ptr<WindowsGraphicsCapture> &instance)
{
    std::scoped_lock lock(allInstancesMutex_);
    allInstances_.remove(instance);
}


void WindowsGraphicsCaptureManager::UpdateFromCaptureThread()
{
    StartInstances();
    RestartInstances();
    StopInstances();
}


void WindowsGraphicsCaptureManager::StartInstances()
{
    std::scoped_lock lock(allInstancesMutex_);

    for (const auto& instance : allInstances_)
    {
        if (instance->CanStart())
        {
            instance->Start();

            if (instance->IsStarted())
            {
                if (std::find(activeInstances_.begin(), activeInstances_.end(), instance) == activeInstances_.end())
                    activeInstances_.push_back(instance);
            }
        }
    }
}


void WindowsGraphicsCaptureManager::RestartInstances()
{
    for (const auto& instance : activeInstances_)
    {
        if (instance->ShouldRestart())
        {
            instance->Restart();
        }
    }
}


void WindowsGraphicsCaptureManager::StopInstances()
{
    for (auto it = activeInstances_.begin(); it != activeInstances_.end();)
    {
        auto instance = *it;
        if (!instance->IsStarted() || instance->ShouldStop())
        {
            instance->Stop();
            it = activeInstances_.erase(it);
            continue;
        }
        ++it;
    }
}


void WindowsGraphicsCaptureManager::StopAllInstances()
{
    for (const auto& instance : activeInstances_)
    {
        instance->Stop();
    }
    activeInstances_.clear();
}
