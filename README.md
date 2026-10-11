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
This fork preserves the v1.1.0 C# API and fixes two native Windows Graphics Capture issues:

- Apply the requested cursor inclusion setting before starting every capture session, including idle/recovery restarts. Live updates are serialized with session creation; failed writes are logged and retried instead of being cached as successful.
- Report the physical visible-frame origin alongside WGC texture dimensions. `GetWindowRect` includes invisible resize borders, whereas WGC captures the DWM visible frame; reporting a zero offset displaced independently rendered cursors. Desktop capture keeps its monitor origin. PrintWindow/BitBlt behavior is unchanged.

Build both shipped architectures with `Tools\Build-Native.ps1` using Visual Studio 2022 C++ tools and a Windows 10/11 SDK. It updates the DLLs under `Assets\uWindowCapture\Plugins`.
Run `Tools\Test-NativeCursor.ps1` to exercise real WGC cursor-setting readback before startup, during live toggles, and after session restarts. The same native tests verify source-origin metadata against DWM bounds using an owned, non-activating window, including negative coordinates and differing raw/physical origins. No Unity or headset session is required.

The source changes live on `feature/windowsar-cursor-session-bounds`; the corresponding Unity package is published on `windowsar-upm`. Consumers should pin the package's commit rather than a moving branch.
