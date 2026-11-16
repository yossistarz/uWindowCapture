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

`1.1.0-windowsar.4` adds opt-in, owned capture mip targets. Call
`UwcWindow.SetMipmapsEnabled(true)` to use a BGRA RenderTexture with a full
trilinear/8x-anisotropic mip chain and an external Texture2D wrapper; false
restores the original mipless Texture2D path. Dispose the wrapper before its
backing resource. `ReleaseCaptureTexture()` withdraws the native target and
releases both pending and published owners.

`UwcSetWindowTexturePtrWithMipOptions(id, pointer, width, height, srgb)` retains
the old valid frame during same-size target replacement. Dimension changes
withdraw readiness. The upload resource stays single-mip. Rendering copies
only subresource zero into a mip target and generates through a cached
format-compatible SRV before publication. Legacy targets use CopyResource.
Generation is skipped only for an already published upload version; this is
not pixel-damage detection. A bitmap/backend rebuild that withdraws a pending
target is recovered by rebinding that same owner, not repeated allocation.

Native checks cover unchanged base pixels, alpha, linear/sRGB mip averaging,
regeneration, target switching and SRV release. Unity allocation and real
capture/readback must also be tested by the consuming application. The
WindowsAR application packages this unpushed source and both architectures
with `Tools\Build-CaptureMipPackage.ps1`; its local UPM tarball is portable
and includes the MIT license. No PackageCache edits are needed.

`1.1.0-windowsar.6` keeps WGC frame-pool draining separate from consumer
capture cadence. Frame-arrival events retain at most one latest frame; replaced
frames are closed immediately and consumed frames remain owned through the GPU
copy/flush. Capture, upload, cursor and icon workers wake on queued work instead
of polling every 100 microseconds. Bounded weighted batches prevent low-priority
starvation, and a request arriving during an upload remains coalesced until the
upload completes. A successful but unchanged source no longer triggers a
one-second session restart; unrequested sessions still stop.
Rebinding a target schedules a render of the retained shared image, so changing
mip/SBS policy does not require an unchanged source to produce another frame.

`Lib.GetWindowCaptureStatistics` reports requests, busy capture attempts, actual
source arrivals, uploads, publications, backend and the published source's QPC
time in 100-nanosecond units. It uses counters, not GPU/CPU pixel readback.
`Tools\Test-NativeCapture.ps1` exercises changing owned windows, real production
workers and mip targets without a headset or a Unity test-only render pump.
Reported native-process CPU includes its controlled source painter; it is not
the CPU cost of the full WindowsAR application.

`1.1.0-windowsar.8` preserves failed WGC objects instead of permanently losing
the monitor GPU path. Both interop creation HRESULTs are checked, and active
Auto captures retry failures after 1, 2, 4, 8, 16, then at most every 30 seconds.
Idle sources and explicitly selected BitBlt/PrintWindow do not drive WGC retries.
Access denial is reported, not bypassed; unsupported systems retain compatibility
capture without repeated activation attempts.

Auto continues compatibility capture until a recovered session produces a real
frame. Desktop CPU/GDI buffers are released on the GPU handoff. Session failures,
closed items and a missing first frame enter bounded recovery; an unchanged source
after its first frame remains valid. Lifecycle timing uses the worker's monotonic
clock, not Unity updates or a main-thread lock around WinRT session teardown.

`Lib.GetWindowWgcStatus` exposes the target handle, last failure HRESULT/stage,
item/session attempts, consecutive failures, recovery count, retry countdown,
availability, session and support state. `WindowCaptureStatusChanged` messages
produce failure/recovery logs on the Unity thread even when verbose diagnostics
are off. Monitor IDs and capture IDs are distinct from the shared desktop HWND.
The existing frame-statistics ABI is unchanged.
Its former reserved slot exposes `capturePending` to distinguish a drained
pipeline from a queued/in-flight request.

Native regressions inject access denial and session failure, verify capped
backoff and explicit-mode/idle behavior, then recover a real monitor. The
throughput runner's `-Windows 1 -Desktop -InitialWgcFailures 2` additionally
requires actual BitBlt and WGC publications during the same capture lifetime.

The `.8` source is maintained on `feature/capture-throughput-stability`.
WindowsAR consumes its portable package with both native architectures; this
does not require changing the upstream-compatible `master` branch.
