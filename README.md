<img src="assets/icon.svg" width="64" height="64" alt="">

# night-screen

[![CI](https://github.com/mustafadroid/night-screen/actions/workflows/ci.yml/badge.svg)](https://github.com/mustafadroid/night-screen/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/mustafadroid/night-screen)](https://github.com/mustafadroid/night-screen/releases)

An eye-care suite in portable **C++26** — blue-light filter, brightness dimmer, eye-break reminders, and focus blackout for **Linux, Windows, and macOS** with zero third-party dependencies.

## Features

| Typical eye-care app | night-screen |
|---|---|
| Blue-light filter (8 presets, temp slider) | `preset` / `temp 1000–6500` + web slider |
| Brightness dimmer incl. below-hardware-minimum | Gamma scaling + fullscreen dim overlay (`brightness 5–100`) |
| Timer / sunrise–sunset / scheduled modes | `Manual`, `Scheduled (HH:MM)`, `SunAuto (lat/lon)` |
| Break reminder 20-20-20, exercise, strict lock | Configurable work/rest, pre-warn notification, strict blackout |
| Focus / black screen / blue screen | `blackout <seconds>` fullscreen dark overlay |
| OLED burn-in guard | `oled on [moveSec]` — dark screen + roaming clock, exits on input |
| Mini window, tray, autostart | Tray icon + control window (Win), web panel `http://127.0.0.1:7788/`, CLI |

## Build

Requires a C++26 compiler: **GCC ≥ 14, Clang ≥ 18, or MSVC 19.40+** (VS 2022 17.10+), plus CMake ≥ 3.28.

Linux needs X11/Xrandr dev headers for real gamma control:

```sh
# Ubuntu/Debian
sudo apt install build-essential cmake libx11-dev libxrandr-dev
# Fedora
sudo dnf install gcc-c++ cmake libX11-devel libXrandr-devel
```

```sh
# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # add -DCMAKE_CXX_COMPILER=clang++ if g++ is too old
cmake --build build -j
ctest --test-dir build

# Windows (Developer PowerShell / VS 2022)
cmake -S . -B build
cmake --build build --config Release
```

Binaries: `build/nightscreen`, `build/nightscreen_tests`
(`build-win/nightscreen.exe` when cross-compiling, see below).

## Windows build on Linux (cross-compile) + run on Windows

No Windows machine needed to *build*: a user-local Zig toolchain cross-compiles
the exact same sources to a static `nightscreen.exe` (system DLLs only).

```sh
# one-time: fetch zig (~54 MB, no root needed) anywhere; tell the wrappers via ZIG_DIR
export ZIG_DIR="$HOME/zig-dl"   # defaults to /tmp/opencode if unset
mkdir -p "$ZIG_DIR" && cd "$ZIG_DIR"
curl -fSLo zig.tar.xz https://ziglang.org/download/0.15.1/zig-x86_64-linux-0.15.1.tar.xz
tar -xf zig.tar.xz   # provides zig-x86_64-linux-0.15.1/zig
cd -  # back to the repo

# build (wrappers in cmake/ pin -target x86_64-windows-gnu -static)
cmake -S . -B build-win --toolchain cmake/toolchain-windows-zig.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j
file build-win/nightscreen.exe   # PE32+ executable (console) x86-64, for MS Windows
```

Ready-made binaries ship with each GitHub release (`nightscreen-windows-x64.zip`
+ `SHA256SUMS.txt` — CI builds and attaches them automatically on version tags).

Copy `nightscreen.exe` to the Windows PC, then in PowerShell/CMD:

```bat
nightscreen.exe daemon
```

A crescent-moon icon appears in the system tray (pure Win32, no installer/DLLs):
- **Double-click** the icon (or right-click → *Open night-screen*) summons the
  summons the main control window: temperature + brightness sliders, preset
  buttons, schedule mode (Manual / Scheduled / Sunrise-sunset), 20-20-20 break
  controls — all applied live.
- **Right-click**: filter on/off, preset submenu, *Take a break now*,
  *Snooze 10 min*, *Start at login*, *Quit*.
- Break reminders arrive as **tray balloons**; strict mode locks the screen
  with the fullscreen overlay.

Then open **http://127.0.0.1:7788/** for the same settings in a browser.
`autostart on` registers `HKCU\...\Run\night-screen`.

> **Windows Defender / SmartScreen:** the exe is unsigned (normal for indie
> open source), so expect a one-time "Unknown publisher → Run anyway" prompt.
> Verify with `SHA256SUMS.txt`, see [SECURITY.md](SECURITY.md) for details.

## Use

```sh
nightscreen daemon [--port 7788]   # filter + breaks + web UI (Ctrl+C stops, restores gamma)
nightscreen status
nightscreen on | off | toggle
nightscreen temp 3400
nightscreen brightness 80
nightscreen preset night          # Off/Reading/Night/Dark Room/Movie/Game/...
nightscreen blackout 60           # fullscreen dark focus break
nightscreen oled on [45]          # OLED burn-in guard: dark screen + clock that
                                  # jumps to a random spot every N sec (any input exits)
nightscreen autostart on|off
nightscreen reset                 # clear filter immediately
```

**Dimming below minimum:** the brightness slider goes to 5%. Below 55% a
software dim overlay fades in (marked in the web UI) — that part works even on
displays whose hardware gamma bottoms out early.

Web panel (while daemon runs): **http://127.0.0.1:7788/** — sliders, presets,
schedule mode, sunrise/sunset lat/lon, 20-20-20 break settings, and an
instant on/off switch for the OLED guard (no Apply needed).
REST: `GET /api/state`, `POST /api/state`.

## Platform notes

- **Linux/X11**: per-CRTC gamma via Xrandr; overlay via override-redirect window.
  Headless (no `$DISPLAY`) still runs (config/CLI/API work, gamma no-ops).
- **Linux/Wayland**: compositors block app gamma; use GNOME Night Light
  (`gsettings set org.gnome.settings-daemon.plugins.color night-light-enabled true`)
  alongside this app for breaks/dimming/scheduling.
- **Windows**: per-display `SetDeviceGammaRamp`; click-through layered overlay;
  autostart via `HKCU\...\Run`. Quitting from the tray — or closing the
  console, logoff, shutdown — always restores the original gamma (SIGHUP is
  handled the same way on Linux/macOS).
- **macOS**: `CGSetDisplayTransferByTable` per display; LaunchAgent autostart.
  Grant Screen Recording if the overlay/break screen is blocked.

Config locations: Linux `~/.config/night-screen/config.json`,
Windows `%APPDATA%\night-screen\config.json`,
macOS `~/Library/Application Support/night-screen/config.json`.

## Layout

```
src/core/      ColorTemp, Config (settings+JSON), SunCalc, FilterEngine, BreakTimer
src/platform/  Gamma (Win/Mac/X11), Overlay (dim/blackout/OLED guard), Notify, Autostart
src/app/       Daemon (apply loop+schedule+HTTP), HttpServer, CLI (main)
web/ui.html    control panel (also embedded in the binary)
tests/         dependency-free core self-checks
```

License: MIT (see LICENSE).
