cmake_minimum_required(VERSION 3.28)

project(night-screen VERSION 1.3.1 LANGUAGES C CXX)

# C++26 everywhere. Requires GCC 14+, Clang 18+, MSVC 19.40+ / VS 2022 17.10+.
# If your default compiler is too old, configure with:
#   CXX=clang++ cmake -B build   (Linux/macOS)
#
# Dialect handling: some CMake versions don't know the C++26 flag mapping for
# every compiler, so when the mapping is absent we pass the flag ourselves (it
# comes after the baseline flag on the command line; compilers honor the last
# -std=... given).
if(DEFINED CMAKE_CXX26_STANDARD_COMPILE_OPTION)
  set(CMAKE_CXX_STANDARD 26)
else()
  set(CMAKE_CXX_STANDARD 23)
  include(CheckCXXCompilerFlag)
  if(MSVC)
    check_cxx_compiler_flag(/std:clatest HAVE_STD_CXX26)
    if(HAVE_STD_CXX26)
      add_compile_options(/std:clatest)
    else()
      message(FATAL_ERROR "MSVC lacks /std:clatest: need VS 2022 17.10+ (MSVC 19.40+).")
    endif()
  else()
    check_cxx_compiler_flag(-std=c++26 HAVE_STD_CXX26)
    if(HAVE_STD_CXX26)
      add_compile_options(-std=c++26)
    else()
      message(FATAL_ERROR "Compiler lacks -std=c++26: need GCC 14+, Clang 18+, Xcode 16+.")
    endif()
  endif()
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /EHsc)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(CORE_SOURCES
  src/core/Config.cpp
  src/core/SunCalc.cpp
  src/core/FilterEngine.cpp
  src/core/BreakTimer.cpp
  src/platform/Gamma.cpp
  src/platform/Notify.cpp
  src/platform/Overlay.cpp
  src/platform/Autostart.cpp
  src/app/HttpServer.cpp
  src/app/Daemon.cpp
)

add_library(nightscreen_core STATIC ${CORE_SOURCES})
target_include_directories(nightscreen_core PUBLIC src)

if(WIN32)
  target_sources(nightscreen_core PRIVATE src/platform/win/TrayGui.cpp)
  target_link_libraries(nightscreen_core PUBLIC gdi32 user32 shell32 ws2_32 comctl32)
  # Identity resources: exe icon + VERSIONINFO (CompanyName etc.).
  # Legit binaries carry these; their absence is a malware-heuristic signal.
  # Forward slashes work for both MSVC rc.exe and llvm-rc.
  set(ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
  set(VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
  set(VERSION_MINOR ${PROJECT_VERSION_MINOR})
  set(VERSION_PATCH ${PROJECT_VERSION_PATCH})
  configure_file(cmake/nightscreen.rc.in "${CMAKE_BINARY_DIR}/nightscreen.rc" @ONLY)
  if(CMAKE_CXX_COMPILER MATCHES "zig")
    # Cross-build: compile resources with LLVM tools into a linkable object.
    find_program(LLVM_RC llvm-rc llvm-rc-20 llvm-rc-19 llvm-rc-18)
    find_program(LLVM_CVTRES llvm-cvtres llvm-cvtres-20 llvm-cvtres-19 llvm-cvtres-18)
    if(LLVM_RC AND LLVM_CVTRES)
      add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/version.o"
        COMMAND "${LLVM_RC}" /FO version.res nightscreen.rc
        COMMAND "${LLVM_CVTRES}" /MACHINE:X64 /OUT:version.o version.res
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS "${CMAKE_BINARY_DIR}/nightscreen.rc" "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico"
        COMMENT "Embedding Windows version info + icon")
      # NOTE: must link into the exe targets, not the static lib — the
      # archiver/linker would otherwise discard the unreferenced object.
      set(WINDOWS_VERSION_OBJ "${CMAKE_BINARY_DIR}/version.o")
    else()
      message(WARNING "llvm-rc/llvm-cvtres not found: exe will lack icon+version info")
    endif()
  else()
    enable_language(RC)
    set(WINDOWS_VERSION_OBJ "${CMAKE_BINARY_DIR}/nightscreen.rc")
  endif()
elseif(APPLE)
  target_link_libraries(nightscreen_core PUBLIC
    "-framework CoreGraphics" "-framework CoreFoundation" "-framework IOKit")
else()
  find_package(X11 REQUIRED)
  target_include_directories(nightscreen_core PUBLIC ${X11_INCLUDE_DIR})
  # Xrandr for per-CRTC gamma. X11::Xrandr target exists on modern CMake.
  find_library(XRANDR_LIB Xrandr)
  if(XRANDR_LIB)
    target_link_libraries(nightscreen_core PUBLIC ${X11_LIBRARIES} ${XRANDR_LIB})
  else()
    target_link_libraries(nightscreen_core PUBLIC ${X11_LIBRARIES})
    target_compile_definitions(nightscreen_core PUBLIC NIGHTSCREEN_NO_XRANDR=1)
  endif()
  target_link_libraries(nightscreen_core PUBLIC pthread)
endif()

add_executable(nightscreen src/app/main.cpp)
target_link_libraries(nightscreen PRIVATE nightscreen_core)
if(WINDOWS_VERSION_OBJ)
  target_sources(nightscreen PRIVATE ${WINDOWS_VERSION_OBJ})
endif()
if(WIN32)
  # GUI subsystem: no console window — the tray icon is the app's face.
  # One-shot CLI commands re-attach to the caller's console (see main.cpp).
  set_target_properties(nightscreen PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MSVC)
    target_link_options(nightscreen PRIVATE /ENTRY:mainCRTStartup)
  else()
    # CMake only adds -mwindows itself for GNU compilers, and zig's clang
    # driver ignores -mwindows — pass the subsystem straight to the linker.
    target_link_options(nightscreen PRIVATE "LINKER:--subsystem,windows")
  endif()
endif()

# Small self-check binary (no test framework needed)
add_executable(nightscreen_tests tests/test_core.cpp)
target_link_libraries(nightscreen_tests PRIVATE nightscreen_core)

enable_testing()
add_test(NAME core COMMAND nightscreen_tests)

install(TARGETS nightscreen RUNTIME DESTINATION bin)
install(FILES web/ui.html DESTINATION share/nightscreen OPTIONAL)
