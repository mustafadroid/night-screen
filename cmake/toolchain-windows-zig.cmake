# Cross-compile for 64-bit Windows using a user-local Zig toolchain.
# Usage:
#   cmake -S . -B build-win --toolchain cmake/toolchain-windows-zig.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win -j
# Requires: /tmp/opencode/zig-x86_64-linux-0.15.1/zig (see README "Windows build on Linux").
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zig-cc.sh")
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zig-c++.sh")

# Never look for host programs in the target env; only target libs/headers.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Skip running test binaries (we are cross-compiling).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
