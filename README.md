uWindowCapture
===================

![MIT License](http://img.shields.io/badge/license-MIT-blue.svg?style=flat)

**uWindowCapture** is an Unity asset to capture windows and make them available in Unity as `Texture2D`.


Screenshots
------------
<img src="https://raw.githubusercontent.com/wiki/hecomi/uWindowCapture/single-window.gif" width="720" /><br />
<img src="https://raw.githubusercontent.com/wiki/hecomi/uWindowCapture/window-object.gif" width="720" /><br />
<img src="https://raw.githubusercontent.com/wiki/hecomi/uWindowCapture/window-manager.gif" width="720" />


Installation
------------
Please download the latest *uWindowCapture.unitypackage* from the [release page](https://github.com/hecomi/uWindowCapture/releases).

Alternatively, add https://github.com/hecomi/uWindowCapture.git#upm to Package Manager.

WindowsAR maintained fork
-------------------------
This fork preserves the v1.1.0 C# API and fixes native capture and texture-lifetime issues:

- Apply the requested cursor inclusion setting before starting every capture session, including idle/recovery restarts. Live updates are serialized with session creation; failed writes are logged and retried instead of being cached as successful.
- Report the physical visible-frame origin alongside WGC texture dimensions. `GetWindowRect` includes invisible resize borders, whereas WGC captures the DWM visible frame; reporting a zero offset displaced independently rendered cursors. Desktop capture keeps its monitor origin. PrintWindow/BitBlt behavior is unchanged.
- Retain Unity textures with owned COM references for windows, icons and the cursor. Each upload/render operation takes a single synchronized snapshot; resource release and graphics work stay outside the short reference lock. Managed window replacement publishes the new texture before destroying the old pending texture, and allocation/publication failure leaves the old texture intact. Render skips a transient size mismatch while the upload thread rebuilds its shared texture.

Build both shipped architectures with `Tools\Build-Native.ps1` using Visual Studio 2022 C++ tools and a Windows 10/11 SDK. It updates the DLLs under `Assets\uWindowCapture\Plugins`.
Run `Tools\Test-NativeCursor.ps1` to exercise real WGC cursor-setting readback before startup, during live toggles, and after session restarts. The same native tests verify source-origin metadata against DWM bounds using an owned, non-activating window, including negative coordinates and differing raw/physical origins. No Unity or headset session is required.
The tests also pause inside the actual window/cursor/icon upload paths, clear the texture and drop the caller's reference, then verify the worker retains it until completion. A concurrent real-D3D replacement/clear test checks eventual resource release. `TEXTURE_COST` compares borrowed versus owned `GetDesc` access in a local microbenchmark; it is not an end-to-end frame-latency measurement.
Real cross-device shared-texture upload/render tests alternate window sizes and verify GPU-readback pixels, including the skipped frame between a target resize and its shared-texture rebuild.

`1.1.0-windowsar.3` also publishes the actual backend of a rendered frame,
bound to its owned Unity texture target. `UwcGetWindowFrameBackend` returns
`None` for allocation-only, replaced, cleared or not-yet-rendered targets.
Managed `frameCaptureMode` / `isFrameReady` use cached target identities, and
pending replacement textures are not exposed until the native render copy
matches them. This prevents delayed old capture messages from publishing an
uninitialized replacement. Frame backend metadata follows the capture/upload
path, not just the requested `Auto` setting. Native GPU tests verify readiness
before/after 32 target replacements and backend identity after each render.
Repeated requests reuse a correctly sized pending managed target until it has
rendered; they must not continually allocate replacements and starve publication.
WindowsAR UPM updates preserve the existing package assembly definitions
(including runtime `autoReferenced=true`) and `Samples~` layout; a raw subtree
of the source asset folder has different assembly-reference defaults.

The source changes live on `feature/windowsar-cursor-session-bounds`; the corresponding Unity package is published on `windowsar-upm`. Consumers should pin the package's commit rather than a moving branch.
