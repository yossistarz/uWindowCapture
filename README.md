uWindowCapture
===================

**uWindowCapture** is an Unity asset to capture windows and make them available in Unity as `Texture2D`.


Screenshots
------------

<img src="https://raw.githubusercontent.com/wiki/hecomi/uWindowCapture/single-window.gif" width="720" /><br />
<img src="https://raw.githubusercontent.com/wiki/hecomi/uWindowCapture/window-object.gif" width="720" /><br />
<img src="https://raw.githubusercontent.com/wiki/hecomi/uWindowCapture/window-manager.gif" width="720" />


Install
-------

- Unity Package
  - Download the latest .unitypackage from [Release page](https://github.com/hecomi/uWindowCapture/releases).
- Git URL (UPM)
  - Add `https://github.com/yossistarz/uWindowCapture.git#windowsar-upm` to Package Manager; pin a commit for reproducible application builds.
- Scoped Registry (UPM)
  - Add a scoped registry to your project.
    - URL: `https://registry.npmjs.com`
    - Scope: `com.hecomi`
  - Install uWindowCapture in Package Manager. 


How to use
----------

Check out the following article (written in Japanese, so please use a translation service).
- https://tips.hecomi.com/entry/2018/08/26/231618

WindowsAR native fixes
----------------------

This package keeps the managed code/assets from upstream UPM commit `0412fe09e11d3720197707a935f8114cc2454639` and replaces both native DLLs with the maintained WindowsAR build.

- Cursor inclusion is applied before every WGC session starts, including idle/recovery restarts.
- WGC window coordinates include the offset to the visible DWM frame, avoiding pointer displacement from invisible resize borders. Desktop monitor origins and PrintWindow/BitBlt behavior are preserved.

Native source, rebuild scripts and real Windows session/bounds regression tests:
https://github.com/yossistarz/uWindowCapture/tree/54240387b54b7b31b467b285e4b05b399162269c

DLL SHA256:
- x86_64: `E7047D518DA4748D3BB4B6D760C9E056EC54259FD2266B0B97FE6A8675B21BE4`
- x86: `DD60794BA0B64EA6264EBEB4CD607C1198C3B588C7F29C9A09196BE6BD0C6415`
