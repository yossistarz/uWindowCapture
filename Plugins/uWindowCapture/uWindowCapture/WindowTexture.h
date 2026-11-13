#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <mutex>
#include <atomic>
#include <cstdint>

#include "Buffer.h"
#include "UnityTextureReference.h"


enum class CaptureMode
{
    None = -1,
    PrintWindow = 0,
    BitBlt = 1,
    WindowsGraphicsCapture = 2,
    Auto = 3,
};


class Window;
class WindowsGraphicsCapture;


class WindowTexture
{
#ifdef UWC_NATIVE_TESTS
friend struct CursorRegressionTests;
#endif
public:
    explicit WindowTexture(Window* window);
    ~WindowTexture();

    void SetUnityTexturePtr(ID3D11Texture2D* ptr, UINT width = 0, UINT height = 0, bool mipSrgb = false);
    ID3D11Texture2D* GetUnityTexturePtr() const;

    void SetCaptureMode(CaptureMode mode);
    CaptureMode GetCaptureMode() const;
    CaptureMode GetRenderedCaptureMode(ID3D11Texture2D* target) const;

    void SetCursorDraw(bool draw);
    bool GetCursorDraw() const;

    UINT GetWidth() const;
    UINT GetHeight() const;
    UINT GetOffsetX() const;
    UINT GetOffsetY() const;

    bool Capture();
    bool Upload();
    bool Render();

    BYTE* GetBuffer();

    UINT GetPixel(int x, int y) const;
    bool GetPixels(BYTE* output, int x, int y, int width, int height) const;

    bool IsWindowsGraphicsCaptureAvailable() const;
    std::shared_ptr<WindowsGraphicsCapture> GetWindowsGraphicsCapture() const;

private:
    CaptureMode GetCaptureModeInternal() const;
    bool IsWindowsGraphicsCapture() const;
    bool CaptureByWin32API(CaptureMode mode);
    void CreateBitmapIfNeeded(HDC hDc, UINT width, UINT height);
    void DeleteBitmap();
    void DrawCursorByWin32API(HWND hWnd, HDC hDcMem, CaptureMode mode);
    bool CaptureByWindowsGraphicsCapture();
    bool RecreateSharedTextureIfNeeded();
    bool UploadByWin32API();
    bool UploadByWindowsGraphicsCapture();

    const Window* const window_;
    std::atomic<CaptureMode> captureMode_ = CaptureMode::Auto;
    std::atomic<CaptureMode> capturedMode_ = CaptureMode::Auto;
    std::weak_ptr<WindowsGraphicsCapture> windowsGraphicsCapture_;
    bool isPrintWindowFailed_ = false;

    UnityTextureReference unityTexture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture_;
    HANDLE sharedHandle_ = nullptr;
    std::mutex sharedTextureMutex_;
    CaptureMode sharedCaptureMode_ = CaptureMode::None;
    mutable std::mutex renderedTextureMutex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> renderedTexture_;
    CaptureMode renderedCaptureMode_ = CaptureMode::None;
    UINT renderedWidth_ = 0, renderedHeight_ = 0;
    uint64_t sharedVersion_ = 0, renderedVersion_ = 0, mipmapGenerationCount_ = 0;
    bool mipSrgb_ = false;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mipView_;

    Buffer<BYTE> buffer_;
    Buffer<BYTE> bufferForGetBuffer_;
    HBITMAP bitmap_ = nullptr;
    std::atomic<UINT> bufferWidth_ = 0;
    std::atomic<UINT> bufferHeight_ = 0;
    std::atomic<UINT> offsetX_ = 0;
    std::atomic<UINT> offsetY_ = 0;
    std::atomic<UINT> textureWidth_ = 0;
    std::atomic<UINT> textureHeight_ = 0;
    std::atomic<bool> drawCursor_ = true;
    mutable std::mutex bufferMutex_;

    float dpiScaleX_ = 1.f;
    float dpiScaleY_ = 1.f;
};