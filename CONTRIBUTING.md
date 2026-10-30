# Contributing to night-screen

Thanks for stopping by. Small, focused PRs are the fastest way to get merged.

## Ground rules

- **C++26**, zero third-party runtime dependencies. The whole point of this
  project is that it builds anywhere with just a toolchain + CMake.
- Platform code lives behind tiny interfaces (`src/platform/*`); shared logic
  stays in `src/core`. Don't leak Win32/X11/Cocoa into core or app code —
  follow the existing `#if defined(_WIN32)` / `__APPLE__` / Linux split.
- Match the existing style: 2-space indent, `snake_case` files, short comments
  that explain *why*, no decorative banners.

## Build & test

```sh
# Linux (needs X11 dev headers for real gamma; headless still builds+runs)
sudo apt install cmake g++ libx11-dev libxrandr-dev   # or clang++
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j && ctest --test-dir build

# Windows (VS 2022 17.10+, MSVC 19.40+) / macOS (Xcode 16+)
cmake -S . -B build
cmake --build build --config Release   # from source: cmake --build build -j

# Cross-compile the Windows exe from Linux (no root needed)
export ZIG_DIR=$PWD/../zig-dl   # folder that will hold zig-x86_64-linux-0.15.1/
mkdir -p "$ZIG_DIR" && cd "$ZIG_DIR"
curl -fSLo zig.tar.xz https://ziglang.org/download/0.15.1/zig-x86_64-linux-0.15.1.tar.xz
tar -xf zig.tar.xz && cd -  # back to repo
sudo apt install llvm       # for llvm-rc / llvm-cvtres (version-info embedding)
cmake -S . -B build-win --toolchain cmake/toolchain-windows-zig.cmake
cmake --build build-win -j
```

New core behavior needs coverage in `tests/test_core.cpp` (plain `assert`s,
no framework). Run the full suite before pushing.

## Pull requests

1. Fork, branch from `main`, keep the diff tight.
2. Update `README.md` if you change user-visible behavior (flags, tray menu,
   config keys, platform notes).
3. CI must be green on all four jobs (Linux, Windows MSVC, macOS, Windows cross).
4. Bump `project(... VERSION ...)` in `CMakeLists.txt` for user-facing changes.

## Releases

Maintainers: tag `vX.Y.Z` on `main` — CI cross-builds `nightscreen.exe`,
zips it with `SHA256SUMS.txt`, and attaches both to the GitHub release.
Update the release notes with behavior changes + the SmartScreen note
(see `SECURITY.md`).

## Security

Don't open public issues for vulnerabilities — use the contact on the repo
owner's profile. False-positive antivirus reports go in a normal issue with
the threat name, engine version, and file SHA-256.
