# Copilot Instructions for H323ASKW / H.323Plus Workspace (macOS ARM64)

This workspace contains a Qt6/macOS H.323 video client built on top of PTLib and H.323 Plus, plus codec/plugins. These instructions capture project-specific architecture, workflows, and conventions so AI coding agents can be immediately productive.

## Big Picture
- Core library layers: PTLib (system abstractions) → H.323 Plus (H.323 stack) → App `h323askw` (client, UI, device integration).
- Media via plugins: video codecs (H.264/H.263/H.261), audio codecs (G.722/G.722.1), camera capture (`vidinput_macos`) loaded at runtime.
- macOS App Bundle target with dynamic plugin discovery; development builds run from `obj_Darwin_aarch64/`.

## Workspace Layout
- App: [h323askw/main.cxx](../../h323askw/main.cxx), [h323askw/Makefile](../../h323askw/Makefile), UI: [h323askw/qt_video_window.cpp](../../h323askw/qt_video_window.cpp), macOS HID: [h323askw/usb_hid_impl.mm](../../h323askw/usb_hid_impl.mm).
- Libraries: [ptlib](../../ptlib) (headers, plugins), [h323plus](../../h323plus) (H.323 stack, build scripts).
- Plugins: [h323plus-plugins](../../h323plus-plugins) and App bundle resources (see `plugins` tree in [h323askw/README.md](../../h323askw/README.md)).
- Docs: [h323askw/docs/INSTALL_GUIDE.md](../../h323askw/docs/INSTALL_GUIDE.md), [h323askw/docs/APP_BUNDLE_BUILD_GUIDE.md](../../h323askw/docs/APP_BUNDLE_BUILD_GUIDE.md), bugfix notes: [h323askw/docs/H323PLUS_BUGFIX_bytesPerFrame.txt](../../h323askw/docs/H323PLUS_BUGFIX_bytesPerFrame.txt).

## Build Workflow (macOS, Apple Silicon)
- Order matters: build `ptlib` → `h323plus` → `h323askw`.
- Typical commands:
  ```bash
  # PTLib
  cd ptlib && ./configure && make
  # H.323 Plus
  cd ../h323plus && ./configure && make
  # App (Qt6 UI optional)
  cd ../h323askw && make video
  ```
- Qt6 (optional UI): install and expose via pkg-config.
  ```bash
  brew install qt
  # Makefile auto-detects Qt6 via pkg-config (Qt6Widgets/Core/Gui)
  ```
- Camera plugin (PTLib AVFoundation):
  ```bash
  cd ptlib/plugins/vidinput_macos && make
  ```

## Plugins and Codecs
- Unified H.264 plugin (preferred): build/copy to app root; Makefile provides manual steps via `make build-h264-unified-plugin` guidance and `deploy-unified-h264-plugin` messaging.
- Separated encoder/decoder (legacy): follow `deploy-h264-plugins` guidance; enforce load order by renaming to `a_...` (decoder) and `z_...` (encoder).
- Runtime plugin discovery: `PTLIBPLUGINDIR` and bundle-relative paths; see dynamic loading in [h323askw/main.cxx](../../h323askw/main.cxx) (`GetDynamicPluginPath()`, `LoadCodecPlugins()` and `LoadVideoPlugins()`).
- For development runs, you can set:
  ```bash
  export PTLIBPLUGINDIR=../h323plus/plugins
  ```

## Run & Debug
- Run from build output:
  ```bash
  ./obj_Darwin_aarch64/h323askw -l -t 4 -o debug.log
  ```
- Common options (see [h323askw/README.md](../../h323askw/README.md)):
  - `-n <address>`: make call, `-l`: listen, `-p <port>`.
  - `--video`, `--no-video`, device selection `-v`, `-s`.
  - Tracing: `-t <level>`, output `-o <file>`.
- App Bundle uses `Contents/Resources/plugins/...` for runtime plugins.

## Project-Specific Conventions
- macOS-specific Objective-C++ files (`.mm`) compiled with custom rule; avoid mixing `.mm` into generic sources lists without considering the Makefile rule.
- Qt6 UI enabled via `USE_QT6` (default on when pkg-config detects Qt6); MOC path is `/opt/homebrew/share/qt/libexec/moc` in the Makefile.
- Link flags are sanitized to remove obsolete or duplicate entries (`ENDLDLIBS`, `LDFLAGS` cleanup in [h323askw/Makefile](../../h323askw/Makefile)).
- Known H.323 Plus issue: `bytesPerFrame` mis-set in plugin manager; fix by commenting lines near 1858/1960 in `h323plus/src/h323pluginmgr.cxx` per [bugfix doc](../../h323askw/docs/H323PLUS_BUGFIX_bytesPerFrame.txt) and rebuild `h323plus`.

## Integration Patterns (Examples)
- Dynamic plugin path resolution and late plugin loading in [h323askw/main.cxx](../../h323askw/main.cxx): sets `PTLIBPLUGINDIR`, enumerates bundle paths, and loads specific `.dylib` files (video/audio).
- macOS AVFoundation capture: build and drop `vidinput_macos_pwplugin.dylib` or `libvidinput_macos.dylib` and allow `LoadVideoPlugins()` to discover.
- USB HID mute button support via IOKit: see [h323askw/usb_hid_impl.mm](../../h323askw/usb_hid_impl.mm) and [h323askw/usb_hid_controller.cpp](../../h323askw/usb_hid_controller.cpp).

## When Editing
- Keep Makefile targets aligned with existing plugin deployment guidance; do not re-enable disabled auto-build steps unless you also update the docs.
- Respect plugin load order semantics (decoder before encoder) when working with separated plugins.
- Prefer adding build/run examples near [h323askw/README.md](../../h323askw/README.md) and docs; link to them from code comments sparingly.

---
Questions or gaps? If any build steps, plugin locations, or Qt6 detection specifics are unclear in your environment, list them and I’ll refine this guide. 