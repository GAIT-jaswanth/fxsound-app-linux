# FxSound for Linux

This directory contains the Linux port of FxSound: a build system, a PipeWire
audio backend that replaces the Windows driver and WASAPI code, and a small
compatibility layer that lets the original DSP engine compile unchanged.

## Requirements

FxSound plugs into **PipeWire**, the default audio server on Ubuntu 22.10 and
newer (and Fedora, Debian 12+, Arch, …). PulseAudio-only systems are not
supported.

## Install the package (Ubuntu 24.04 or newer)

```sh
sudo apt install ./fxsound_1.2.14-2_amd64.deb
```

Then start **FxSound** from the app menu, or run `fxsound`.

The prebuilt package targets Ubuntu 24.04+ (it was built against that
release's libraries). On older releases, build from source as below.

## Build from source

```sh
sudo apt install build-essential cmake ninja-build pkg-config git \
    libpipewire-0.3-dev libspa-0.2-dev \
    libfreetype-dev libx11-dev libxext-dev libxrandr-dev libxinerama-dev \
    libxcursor-dev libgl-dev

cmake -S linux -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build

# run without installing
./build/FxSound_artefacts/Release/FxSound

# or make a .deb
(cd build && cpack)
```

JUCE 6.1.6 (the version the Windows build uses) is downloaded automatically.
To use an existing checkout, add `-DFX_JUCE_DIR=/path/to/JUCE`.

## How it works on Linux

On Windows, FxSound installs a virtual audio driver. On Linux no driver is
needed: FxSound creates a PipeWire output device called
**"FxSound Audio Enhancer"**, makes it the system default while processing is
on, runs everything played into it through the FxSound DSP, and plays the
result on the output you select in FxSound. Turning FxSound off (or quitting
it) hands the default back to your real output. If FxSound crashes or is
killed, PipeWire falls back to your real output automatically.

Choosing a different output in your desktop's sound settings while FxSound is
on makes FxSound play through that output, as on Windows.

## Differences from Windows

- **Global hotkeys are not available.** Linux has no portable global-hotkey
  API (Wayland does not allow global key grabs), so the hotkey settings are
  hidden. You can bind desktop shortcuts to command-line options instead, e.g.
  `fxsound --power=0` / `fxsound --power=1`.
- **System tray:** the icon is a StatusNotifierItem (the D-Bus tray protocol
  used by Ubuntu's AppIndicator extension, KDE Plasma, Cinnamon, waybar, and
  XFCE/MATE with their status-notifier plugins). Ubuntu shows it out of the
  box; other GNOME installs need the *AppIndicator and KStatusNotifierItem
  Support* extension, and the icon appears as soon as it is enabled. Left
  click shows/hides the window; the menu is the same as on Windows. On
  desktops with no StatusNotifierItem host, FxSound falls back to an XEmbed
  icon, but never on GNOME: GNOME Shell treats any X error while embedding a
  tray window as fatal and ends the session (this happened with 1.2.14-1).
  `FXSOUND_TRAY=sni|xembed|none` overrides the choice. Without a tray, run
  `fxsound` again to bring the window back.
- **Launch at startup** creates `~/.config/autostart/fxsound.desktop`.
- **Settings, presets and logs** live in `~/.config/FxSound/`.
- **Command-line options** (`docs/COMMAND_LINE_OPTIONS.md`) work and are
  forwarded to the running FxSound. `fxsound --status` prints its JSON in the
  terminal you run it from.

## What is in this directory

| Path | Purpose |
|---|---|
| `CMakeLists.txt` | Build for the DSP library, audio backend, GUI app, tests and `.deb` |
| `audiopassthru/` | PipeWire implementation of `AudioPassthru` (same API as Windows) |
| `compat/` | Win32 compatibility headers for the DSP engine; `dsp_prelude.h` gives the DSP Windows' 32-bit `long` |
| `dsp_support/` | Linux versions of the file/string/registry helpers the DSP uses |
| `gui/` | Linux `SysInfo`, single-instance command forwarding, and the StatusNotifierItem tray icon |
| `packaging/` | Desktop entry and icon |
| `tests/` | DSP and end-to-end audio tests (see below) |
| `check_dsp_prelude.py` | Verifies no system header is affected by the `long` remap |
| `fix_include_case.py` | Fixes `#include` letter case (Linux filenames are case-sensitive) |

## Porting notes for maintainers

- **`long` is 32-bit on Windows, 64-bit on Linux.** The DSP stores floats in
  `long` arrays and depends on 32-bit `long` throughout (~1,350 uses).
  Instead of rewriting it, `compat/dsp_prelude.h` is force-included into DSP
  sources: it includes all system headers, then `#define long int`.
  `check_dsp_prelude.py` verifies that no system header is parsed after the
  remap. Constructs it can't handle (`long long`) fail to compile rather than
  misbehave.
- **Wide `printf`/`scanf` `%s`** means `wchar_t*` on Windows but `char*` in
  glibc. `compat/win_compat.cpp` wraps these functions to keep Windows
  semantics for the DSP.
- **The Windows registry** (`HKCU\Software\DFX`) is replaced by
  `~/.config/FxSound/dsp_registry.txt`.
- Changes to shared GUI/DSP sources are guarded with `JUCE_WINDOWS`/`_WIN32`
  or are portable rewrites that produce identical results on Windows.

## Tests

```sh
cmake --build build --target dsp_test eq_response_test dsp_process_raw passthru_test
./build/dsp_test 1.fac        # DSP sanity (run from the repo root): processing, bypass
./build/eq_response_test                         # response follows the preset EQ curve
python3 linux/check_dsp_prelude.py               # long-remap invariant
```

`tests/run_e2e.sh` plus `tests/compare_capture.py` play a tone through a live
PipeWire session (FxSound sink → DSP → output), record the output, and compare
it with the offline DSP result.

Tray icon tests (need `python3-gi`; the second also `pipewire`, `wireplumber`,
`xvfb`, `xdotool` and `pip install python-xlib`):

```sh
cmake --build build --target sni_test
dbus-run-session -- python3 linux/tests/sni_test.py build/sni_test        # D-Bus protocol, fake tray host
dbus-run-session -- python3 linux/tests/tray_app_test.py build/FxSound_artefacts/Release/FxSound
```

`tray_app_test.py` runs the real app with fake GNOME/KDE/XFCE tray hosts and
checks the menu actions, and that the app never asks GNOME to embed an X11
tray window (and docks the XEmbed fallback only once elsewhere).

## Known limitations

- Multichannel (5.1/7.1) output is processed as stereo; PipeWire up/down-mixes.
- In automated tests in a container, occasional short dropouts appeared
  (both with and without FxSound in the chain); this has not been
  investigated on real hardware yet.
- The version reported is 1.2.14.0, from `fxsound/JuceLibraryCode/JuceHeader.h`
  (the `.jucer` file says 1.2.15.0; the Windows build has the same mismatch).
