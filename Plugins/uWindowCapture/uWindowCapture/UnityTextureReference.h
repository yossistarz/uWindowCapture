#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <mutex>

class UnityTextureReference
{
public:
    void Set(ID3D11Texture2D* texture)
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> replacement = texture;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            texture_.Swap(replacement);
        }
        // Releasing the previous resource can be expensive; do it outside the lock.
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> Acquire() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return texture_;
    }

    // Borrowed pointer for the existing C API; native graphics work must use Acquire().
    ID3D11Texture2D* Get() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return texture_.Get();
    }

private:
    mutable std::mutex mutex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
};
