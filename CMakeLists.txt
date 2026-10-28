cmake_minimum_required(VERSION 3.25)
project(MooClient VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "MooClient is a Windows application.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(WEBVIEW2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2" CACHE PATH "WebView2 SDK root")
set(WEBVIEW2_INCLUDE "${WEBVIEW2_ROOT}/build/native/include")
set(WEBVIEW2_LIB "${WEBVIEW2_ROOT}/build/native/x64/WebView2LoaderStatic.lib")

if(NOT EXISTS "${WEBVIEW2_INCLUDE}/WebView2.h")
  message(FATAL_ERROR
    "WebView2 SDK not found.\n"
    "Run: powershell -ExecutionPolicy Bypass -File cmake/bootstrap_deps.ps1")
endif()

set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I\"${CMAKE_CURRENT_SOURCE_DIR}/res\"")
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I\"${CMAKE_CURRENT_BINARY_DIR}/generated\"")

# One source of truth for the version: CMakeLists -> generated/version.h -> both
# the C++ build and the VERSIONINFO resource.
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/res/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
  @ONLY)

add_executable(MooClient WIN32
  src/main.cpp
  src/shell/window.cpp
  src/shell/single_instance.cpp
  src/shell/job.cpp
  src/shell/crash.cpp
  src/shell/splash.cpp
  src/webview/webview_host.cpp
  src/webview/nav_policy.cpp
  src/cache/asset_cache.cpp
  src/cache/asset_policy.cpp
  src/cache/sha256.cpp
  src/update/json.cpp
  src/update/updater.cpp
  src/diag/log.cpp
  res/app.rc
)

# CMake's MSVC link wrapper owns manifest generation. Let it produce the default
# manifest, then replace the embedded resource with ours (DPI, comctl v6, UAC).
if(CMAKE_MT)
  add_custom_command(TARGET MooClient POST_BUILD
    COMMAND "${CMAKE_MT}" -nologo
            -manifest "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest"
            -outputresource:$<TARGET_FILE:MooClient>
    COMMENT "Embedding application manifest"
    VERBATIM)
endif()

target_include_directories(MooClient PRIVATE src res "${CMAKE_CURRENT_BINARY_DIR}/generated"
                                                    "${WEBVIEW2_INCLUDE}")
target_compile_definitions(MooClient PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00)

if(MSVC)
  target_compile_options(MooClient PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
  # The static loader is built for the release CRT; this also keeps the app a
  # single self-contained exe (no VC++ redistributable).
endif()

target_link_libraries(MooClient PRIVATE
  "${WEBVIEW2_LIB}"
  bcrypt
  comctl32
  crypt32
  dbghelp
  dwmapi
  netapi32
  ole32
  oleaut32
  shell32
  shlwapi
  urlmon
  user32
  userenv
  version
  winhttp
  windowscodecs
  gdi32
  advapi32
)

set_target_properties(MooClient PROPERTIES
  OUTPUT_NAME "MooClient"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
