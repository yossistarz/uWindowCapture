using System;
using UnityEngine;
using Object = UnityEngine.Object;

namespace uWindowCapture
{
    internal sealed class CaptureTextureTarget : IDisposable
    {
        public Texture2D Texture { get; private set; }
        public RenderTexture Backing { get; private set; }
        public IntPtr Pointer { get; private set; }
        public bool Mipmaps { get; private set; }

        public CaptureTextureTarget(int width, int height, bool mipmaps)
        {
            Mipmaps = mipmaps;
            try
            {
                if (mipmaps)
                {
                    Backing = new RenderTexture(width, height, 0, RenderTextureFormat.BGRA32, RenderTextureReadWrite.Default) {
                        name = "uWindowCapture mip-capable capture target", useMipMap = true, autoGenerateMips = false,
                        filterMode = FilterMode.Trilinear, anisoLevel = 8, wrapMode = TextureWrapMode.Clamp
                    };
                    if (!Backing.Create()) throw new InvalidOperationException("The mip-capable capture target could not be allocated.");
                    Pointer = Backing.GetNativeTexturePtr();
                    Texture = Texture2D.CreateExternalTexture(width, height, TextureFormat.BGRA32, true, !Backing.sRGB, Pointer);
                    Texture.filterMode = FilterMode.Trilinear; Texture.anisoLevel = 8; Texture.wrapMode = TextureWrapMode.Clamp;
                }
                else
                {
                    Texture = new Texture2D(width, height, TextureFormat.BGRA32, false);
                    Pointer = Texture.GetNativeTexturePtr();
                }
                if (Pointer == IntPtr.Zero) throw new InvalidOperationException("The capture target has no native resource.");
            }
            catch { Dispose(); throw; }
        }

        public bool Matches(int width, int height, bool mipmaps)
            => Texture != null && Texture.width == width && Texture.height == height && Mipmaps == mipmaps;

        public void Bind(int windowId)
            => Lib.SetWindowTexturePtrWithMipOptions(windowId, Pointer, (uint)Texture.width, (uint)Texture.height,
                Backing != null && Backing.sRGB ? 1 : 0);

        public void Dispose()
        {
            if (Texture != null) { Object.DestroyImmediate(Texture); Texture = null; }
            if (Backing != null) { Backing.Release(); Object.DestroyImmediate(Backing); Backing = null; }
            Pointer = IntPtr.Zero;
        }
    }
}
