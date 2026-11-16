#pragma once

#include <functional>
#include <chrono>
#include <mutex>
#include <set>
#include <list>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <dxgi.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.Capture.h>

enum class WgcFailureStage : int32_t
{
    None = 0, CreateItem = 1, StartSession = 2, ItemClosed = 3, ReadFrame = 4, FirstFrame = 5
};

struct WgcCaptureStatus
{
    uint64_t itemAttempts = 0, sessionAttempts = 0, recoveries = 0, retryAfterMilliseconds = 0, targetHandle = 0;
    int32_t lastError = 0, failureStage = 0, consecutiveFailures = 0, available = 0, started = 0, supported = 0;
};
static_assert(sizeof(WgcCaptureStatus) == 64 && offsetof(WgcCaptureStatus, lastError) == 40,
    "WGC status must preserve the managed ABI on both architectures.");

class WindowsGraphicsCapture
    : public std::enable_shared_from_this<WindowsGraphicsCapture>
{
friend class WindowsGraphicsCaptureManager;
#ifdef UWC_NATIVE_TESTS
friend struct CursorRegressionTests;
friend struct CaptureThroughputTests;
#endif
public:
    static bool IsSupported();
    static bool IsCursorCaptureEnabledApiSupported();

public:
    struct Result
    {
        ID3D11Texture2D *pTexture = nullptr;
        int width = 0;
        int height = 0;
        bool hasSizeChanged = false;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame = nullptr;
        int64_t sourceTime = 0;
        void Close() noexcept;
    };

    explicit WindowsGraphicsCapture(HWND hWnd);
    explicit WindowsGraphicsCapture(HMONITOR hMonitor);
    ~WindowsGraphicsCapture();
    int GetHeight() const { return size_.Height; }
    int GetWidth() const { return size_.Width; }
    void RequestStart();
    bool IsAvailable() const;
    bool IsStarted() const { return isStarted_; }
    void EnableCursorCapture(bool enabled);
    Result TryGetLatestResult();
    void SetFrameAvailableCallback(std::function<void()> callback);
    void SetStatusChangedCallback(std::function<void()> callback);
    WgcCaptureStatus GetStatus() const;
    bool IsWaitingForFrame() const { return isStarted_ && frameRequested_; }
    uint64_t GetArrivedFrameCount() const { return arrivedFrames_; }
    void ReleaseLatestResult();
    void ReleaseFrame();
    void ChangePoolSize(int width, int height);
    const wchar_t * GetDisplayName() const;

private:
    bool ShouldStop() const;
    bool ShouldRestart() const;
    bool CanStart() const;
    void Restart();
    void Start();
    void Stop();
    bool CreateItem();
    bool CreatePoolAndSession();
    void DestroyPoolAndSession();
    void OnFrameArrived(const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& sender, uint64_t generation);
    void RecordFailure(HRESULT error, WgcFailureStage stage);
    void NotifyStatusChanged();
    void RevokeItemClosed();

    const HWND hWnd_;
    const HMONITOR hMonitor_;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_ = nullptr;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_ = nullptr;
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_ = nullptr;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame_ = nullptr;
    winrt::Windows::Graphics::SizeInt32 size_ = { 0, 0 };
    mutable std::mutex itemMutex_;
    std::mutex sessionAndPoolMutex_;
    std::mutex frameMutex_;
    std::atomic<bool> isStarted_ = false;
    std::atomic<bool> isCursorCaptureEnabled_ = { true };
    std::atomic<bool> isStartRequested_ = { false };
    std::atomic<bool> isRestartRequested_ = { false };
    std::atomic<bool> hasReceivedFrame_ = false;
    std::atomic<bool> frameRequested_ = false;
    winrt::event_token frameArrivedToken_{};
    std::function<void()> frameAvailableCallback_;
    std::function<void()> statusChangedCallback_;
    std::atomic<bool> itemReady_ = false, recovering_ = false, itemClosed_ = false;
    std::atomic<uint64_t> itemAttempts_ = 0, sessionAttempts_ = 0, recoveries_ = 0;
    std::atomic<uint64_t> retryAt_ = 0, lastRequestAt_ = 0, sessionStartedAt_ = 0, poolGeneration_ = 0;
    std::atomic<int32_t> lastError_ = S_OK, failureStage_ = 0, consecutiveFailures_ = 0;
    winrt::event_token itemClosedToken_{};
    bool itemClosedSubscribed_ = false;
    std::atomic<uint64_t> arrivedFrames_ = 0;
#ifdef UWC_NATIVE_TESTS
    std::atomic<uint64_t> drainedFrames_ = 0;
    std::atomic<uint64_t> emptyReads_ = 0;
    inline static std::function<HRESULT()> createItemFailureForTests_;
    inline static std::function<HRESULT()> startSessionFailureForTests_;
#endif
};


class WindowsGraphicsCaptureManager final
{
#ifdef UWC_NATIVE_TESTS
friend struct CursorRegressionTests;
#endif
public:
    std::shared_ptr<WindowsGraphicsCapture> Create(HWND hWnd);
    std::shared_ptr<WindowsGraphicsCapture> Create(HMONITOR hMonitor);
    void Destroy(const std::shared_ptr<WindowsGraphicsCapture>& instance);

    void UpdateFromCaptureThread();

    void StopAllInstances();
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice & GetDevice();

private:
    void StartInstances();
    void RestartInstances();
    void StopInstances();

    using Ptr = std::shared_ptr<WindowsGraphicsCapture>;
    std::list<Ptr> allInstances_;
    std::list<Ptr> activeInstances_;
    std::mutex allInstancesMutex_;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice deviceWinRt_ = nullptr;
};