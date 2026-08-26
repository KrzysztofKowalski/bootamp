# bootamp — C++23 + SIMD rewrite of cliamp.
# Single foreground process, no daemon/IPC. miniaudio + FFTW3f + libswresample.
cmake_minimum_required(VERSION 3.20)

project(bootamp
  VERSION 0.1.0
  DESCRIPTION "Retro terminal music player (C++23 + SIMD rewrite of cliamp)"
  LANGUAGES CXX)

# NOTE: we do NOT set CMAKE_CXX_STANDARD to 2c — CMake 4.4's try_compile (used by
# find_package(Threads) via google-benchmark) rejects the unknown "CXX2c"
# dialect. Instead CompilerFlags.cmake forces -std=c++2c globally via
# add_compile_options, which applies to every target defined below.
set(CMAKE_CXX_STANDARD 23)        # closest CMake-known dialect; overridden by -std=c++2c
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Build modes -------------------------------------------------------------
option(BOOTAMP_ENABLE_TSAN "Enable ThreadSanitizer on audio/radio/ytdl tests" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerFlags)   # -std=c++2c -O3 -march=x86-64-v3 (NO -ffast-math)
include(SimdDispatch)     # runtime __builtin_cpu_supports("avx2") helpers

# --- Required dependencies (pkg-config) --------------------------------------
find_package(PkgConfig REQUIRED)

pkg_check_modules(SWRESAMPLE   REQUIRED IMPORTED_TARGET libswresample)
pkg_check_modules(AVUTIL       REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(FFTW3F       REQUIRED IMPORTED_TARGET fftw3f)
pkg_check_modules(SNDFILE       REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(VORBIS        REQUIRED IMPORTED_TARGET vorbis)
pkg_check_modules(VORBISFILE    REQUIRED IMPORTED_TARGET vorbisfile)
pkg_check_modules(OGG           REQUIRED IMPORTED_TARGET ogg)
pkg_check_modules(OPUS          REQUIRED IMPORTED_TARGET opus)
pkg_check_modules(FLAC          REQUIRED IMPORTED_TARGET flac)
pkg_check_modules(SPDLOG       REQUIRED IMPORTED_TARGET spdlog)
pkg_check_modules(FMT           REQUIRED IMPORTED_TARGET fmt)
pkg_check_modules(TAGLIB        REQUIRED IMPORTED_TARGET taglib)
pkg_check_modules(CURL          REQUIRED IMPORTED_TARGET libcurl)

# --- Required dependencies (find_package, header-only / cmake config) -------
find_package(nlohmann_json REQUIRED)
find_package(tomlplusplus REQUIRED)
find_package(OpenSSL REQUIRED)  # TLS for https radio streams (audio/http_socket.cpp)
find_package(Catch2 REQUIRED)

# CLI11 — header-only command-line parser (may be system or fetched).
find_package(CLI11 QUIET)
if(NOT CLI11_FOUND)
  find_path(CLI11_INCLUDE_DIR CLI/CLI.hpp)
  if(CLI11_INCLUDE_DIR)
    set(CLI11_FOUND TRUE)
    add_library(CLI11::CLI11 INTERFACE IMPORTED)
    set_target_properties(CLI11::CLI11 PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${CLI11_INCLUDE_DIR}")
  else()
    message(WARNING "CLI11 not found — the bootamp executable target will be skipped. "
                     "Install cli11 (pacman -S cli11) or fetch the single-header CLI.hpp.")
  endif()
endif()

# --- Optional backends -------------------------------------------------------
# miniaudio (pacman: miniaudio) — audio sink. Gates bootamp_audio_sink_miniaudio
# and the executable. If absent, only NullSink is available.
# Arch's miniaudio package installs the header under /usr/include/miniaudio/,
# so search with PATH_SUFFIXES.
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h PATH_SUFFIXES miniaudio)
if(MINIAUDIO_INCLUDE_DIR)
  set(BOOTAMP_HAS_MINIAUDIO TRUE)
  message(STATUS "miniaudio found at ${MINIAUDIO_INCLUDE_DIR}")
else()
  set(BOOTAMP_HAS_MINIAUDIO FALSE)
  message(WARNING "miniaudio.h not found — miniaudio audio sink and the bootamp "
                  "executable will be skipped. Install with: sudo pacman -S miniaudio")
endif()

# FTXUI (AUR: ftxui) — terminal UI. Gates bootamp_ui_ftxui and the executable.
find_path(FTXUI_INCLUDE_DIR ftxui/component/screen_interactive.hpp)
find_library(FTXUI_COMPONENT_LIB  NAMES ftxui-component)
find_library(FTXUI_DOM_LIB        NAMES ftxui-dom)
find_library(FTXUI_SCREEN_LIB     NAMES ftxui-screen)
if(FTXUI_INCLUDE_DIR AND FTXUI_COMPONENT_LIB AND FTXUI_DOM_LIB AND FTXUI_SCREEN_LIB)
  set(BOOTAMP_HAS_FTXUI TRUE)
  message(STATUS "FTXUI found at ${FTXUI_INCLUDE_DIR}")
else()
  set(BOOTAMP_HAS_FTXUI FALSE)
  message(WARNING "FTXUI not found — UI targets and the bootamp executable will be "
                  "skipped. Install with: paru -S ftxui")
endif()

# --- Source layout -----------------------------------------------------------
file(GLOB FOUNDATION_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/foundation/*.cpp")
file(GLOB CONFIG_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/config/*.cpp")
file(GLOB DSP_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/dsp/*.cpp")
file(GLOB AUDIO_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/audio/*.cpp")
file(GLOB PLAYLIST_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/playlist/*.cpp")
file(GLOB RESOLVE_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/resolve/*.cpp")
file(GLOB PROVIDER_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/provider/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/provider/radio/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/provider/local/*.cpp")
file(GLOB UI_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/ui/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/ui/vis_drivers/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/ui/screens/*.cpp")
file(GLOB APP_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/app/*.cpp")

# Common include + library wiring. All targets link against the same set of
# audio deps so headers can reference fftw3.h / sndfile.h / swresample.h freely.
set(BOOTAMP_COMMON_INCLUDES
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}"
  ${FFTW3F_INCLUDE_DIRS}
  ${SNDFILE_INCLUDE_DIRS}
  ${VORBIS_INCLUDE_DIRS}
  ${OGG_INCLUDE_DIRS}
  ${OPUS_INCLUDE_DIRS}
  ${FLAC_INCLUDE_DIRS}
  ${SWRESAMPLE_INCLUDE_DIRS}
  ${TAGLIB_INCLUDE_DIRS}
  ${CURL_INCLUDE_DIRS})
if(BOOTAMP_HAS_MINIAUDIO)
  list(APPEND BOOTAMP_COMMON_INCLUDES ${MINIAUDIO_INCLUDE_DIR})
endif()

set(BOOTAMP_COMMON_LIBS
  PkgConfig::FFTW3F
  PkgConfig::SWRESAMPLE
  PkgConfig::AVUTIL
  PkgConfig::SNDFILE
  PkgConfig::VORBIS
  PkgConfig::VORBISFILE
  PkgConfig::OGG
  PkgConfig::OPUS
  PkgConfig::FLAC
  PkgConfig::SPDLOG
  PkgConfig::FMT
  PkgConfig::TAGLIB
  PkgConfig::CURL
  nlohmann_json::nlohmann_json
  tomlplusplus::tomlplusplus)

# Helper: declare a bootamp library with common includes/libs. When source
# files exist (added later by other agents) the target is STATIC; while the
# tree is header-only the target is INTERFACE so configure succeeds with no
# .cpp files yet present (the GLOBs above return empty lists). INTERFACE
# targets only accept INTERFACE-scope properties, so we branch on the
# target type via the TARGET_TYPE property.
function(bootamp_lib name)
  set(_srcs ${ARGN})
  list(FILTER _srcs EXCLUDE REGEX "^\\$<")  # drop empty generator exprs
  if(_srcs)
    add_library(${name} STATIC ${_srcs})
  else()
    add_library(${name} INTERFACE)
  endif()
  get_target_property(_type ${name} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    target_include_directories(${name} INTERFACE ${BOOTAMP_COMMON_INCLUDES})
    target_link_libraries(${name}      INTERFACE ${BOOTAMP_COMMON_LIBS})
    if(BOOTAMP_HAS_MINIAUDIO)
      target_compile_definitions(${name} INTERFACE BOOTAMP_HAS_MINIAUDIO=1)
    endif()
    if(BOOTAMP_HAS_FTXUI)
      target_compile_definitions(${name} INTERFACE BOOTAMP_HAS_FTXUI=1)
    endif()
  else()
    target_include_directories(${name} PUBLIC ${BOOTAMP_COMMON_INCLUDES})
    target_link_libraries(${name}      PUBLIC ${BOOTAMP_COMMON_LIBS})
    if(BOOTAMP_HAS_MINIAUDIO)
      target_compile_definitions(${name} PUBLIC BOOTAMP_HAS_MINIAUDIO=1)
    endif()
    if(BOOTAMP_HAS_FTXUI)
      target_compile_definitions(${name} PUBLIC BOOTAMP_HAS_FTXUI=1)
    endif()
  endif()
endfunction()

# --- Libraries (interfaces defined by headers in src/) ----------------------
bootamp_lib(bootamp_foundation ${FOUNDATION_SRCS})
bootamp_lib(bootamp_config      ${CONFIG_SRCS})
bootamp_lib(bootamp_dsp         ${DSP_SRCS})
bootamp_lib(bootamp_audio       ${AUDIO_SRCS})
bootamp_lib(bootamp_playlist    ${PLAYLIST_SRCS})
bootamp_lib(bootamp_resolve     ${RESOLVE_SRCS})
bootamp_lib(bootamp_provider    ${PROVIDER_SRCS})
bootamp_lib(bootamp_ui          ${UI_SRCS})

# bootamp_ui contains the real FTXUI frontend (ftxui_app.cpp + screens/*.cpp),
# so it must expose the FTXUI include dir and link the FTXUI libraries PUBLIC:
# every consumer (the bootamp executable and test_ui) needs the ftxui::
# headers and symbols. The executable's explicit FTXUI link below is then
# redundant but harmless.
if(BOOTAMP_HAS_FTXUI)
  get_target_property(_ui_type bootamp_ui TYPE)
  if(_ui_type STREQUAL "INTERFACE_LIBRARY")
    target_include_directories(bootamp_ui INTERFACE ${FTXUI_INCLUDE_DIR})
    target_link_libraries(bootamp_ui INTERFACE ${FTXUI_COMPONENT_LIB} ${FTXUI_DOM_LIB} ${FTXUI_SCREEN_LIB})
  else()
    target_include_directories(bootamp_ui PUBLIC ${FTXUI_INCLUDE_DIR})
    target_link_libraries(bootamp_ui PUBLIC ${FTXUI_COMPONENT_LIB} ${FTXUI_DOM_LIB} ${FTXUI_SCREEN_LIB})
  endif()
endif()

# OpenSSL TLS for https radio streams — bootamp_audio only (http_socket.cpp).
# Branch on target type like the FTXUI block above: INTERFACE targets only
# accept INTERFACE-scope usage requirements; STATIC targets need PUBLIC so
# the target's own compilation of http_socket.cpp sees the OpenSSL include
# dirs and its consumers link libssl/libcrypto.
get_target_property(_audio_type bootamp_audio TYPE)
if(_audio_type STREQUAL "INTERFACE_LIBRARY")
  target_link_libraries(bootamp_audio INTERFACE OpenSSL::SSL OpenSSL::Crypto)
else()
  target_link_libraries(bootamp_audio PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# Inter-library dependencies (header-level). Use INTERFACE scope so this
# works whether the target is INTERFACE (header-only, today) or STATIC
# (once other agents add .cpp sources): INTERFACE lib requirements propagate
# to consumers regardless of the target type, and STATIC targets accept
# INTERFACE linkage too.
target_link_libraries(bootamp_foundation INTERFACE)
target_link_libraries(bootamp_config      INTERFACE bootamp_foundation)
target_link_libraries(bootamp_dsp         INTERFACE bootamp_foundation)
target_link_libraries(bootamp_audio       INTERFACE bootamp_dsp bootamp_foundation bootamp_playlist)
target_link_libraries(bootamp_playlist   INTERFACE bootamp_foundation)
target_link_libraries(bootamp_resolve    INTERFACE bootamp_playlist bootamp_audio)
target_link_libraries(bootamp_provider    INTERFACE bootamp_playlist bootamp_resolve)
target_link_libraries(bootamp_ui          INTERFACE bootamp_dsp bootamp_audio bootamp_resolve)

# --- Tests (Catch2, header-only — no linking of app code needed) -------------
enable_testing()
file(GLOB TEST_FOUNDATION_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/tests/foundation/*.cpp")
file(GLOB TEST_DSP_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/tests/dsp/*.cpp")
# bench_dsp.cpp is a google-benchmark target, not a Catch2 test — keep it out
# of the test_dsp executable (it links benchmark::benchmark separately).
list(FILTER TEST_DSP_SRCS EXCLUDE REGEX "bench_dsp")
file(GLOB TEST_AUDIO_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/tests/audio/*.cpp")
file(GLOB TEST_PLAYLIST_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/tests/playlist/*.cpp")
file(GLOB TEST_RESOLVE_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/tests/resolve/*.cpp")
file(GLOB TEST_PROVIDER_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/tests/provider/*.cpp")
file(GLOB TEST_CONFIG_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/tests/config/*.cpp")
file(GLOB TEST_UI_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/tests/ui/*.cpp")

function(bootamp_test name srcs libs)
  if(NOT srcs)
    message(STATUS "no sources for test ${name} — skipping")
    return()
  endif()
  add_executable(${name} ${srcs})
  target_include_directories(${name} PRIVATE ${BOOTAMP_COMMON_INCLUDES})
  target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain ${libs})
  if(BOOTAMP_ENABLE_TSAN)
    target_compile_options(${name} PRIVATE -fsanitize=thread -g)
    target_link_options(${name}    PRIVATE -fsanitize=thread)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

bootamp_test(test_foundation "${TEST_FOUNDATION_SRCS}" "bootamp_foundation")
bootamp_test(test_dsp        "${TEST_DSP_SRCS}"        "bootamp_dsp;bootamp_audio")
bootamp_test(test_audio      "${TEST_AUDIO_SRCS}"      "bootamp_audio")
bootamp_test(test_playlist  "${TEST_PLAYLIST_SRCS}"   "bootamp_playlist")
bootamp_test(test_resolve    "${TEST_RESOLVE_SRCS}"    "bootamp_resolve;bootamp_audio")
bootamp_test(test_provider   "${TEST_PROVIDER_SRCS}"   "bootamp_provider")
bootamp_test(test_config     "${TEST_CONFIG_SRCS}"     "bootamp_config")
bootamp_test(test_ui         "${TEST_UI_SRCS}"         "bootamp_ui")

# --- Benchmark (google-benchmark) -------------------------------------------
find_package(benchmark QUIET)
if(benchmark_FOUND)
  file(GLOB BENCH_SRCS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tests/dsp/bench_dsp.cpp")
  if(BENCH_SRCS)
    add_executable(bench_dsp ${BENCH_SRCS})
    target_include_directories(bench_dsp PRIVATE ${BOOTAMP_COMMON_INCLUDES})
    target_link_libraries(bench_dsp PRIVATE bootamp_dsp benchmark::benchmark)
  endif()
else()
  message(STATUS "google-benchmark not found — bench_dsp skipped")
endif()

# --- Executable (requires miniaudio + FTXUI + CLI11) -------------------------
if(BOOTAMP_HAS_MINIAUDIO AND BOOTAMP_HAS_FTXUI AND CLI11_FOUND)
  add_executable(bootamp ${APP_SRCS})
  target_include_directories(bootamp PRIVATE ${BOOTAMP_COMMON_INCLUDES})
  target_link_libraries(bootamp PRIVATE
    bootamp_foundation bootamp_config bootamp_dsp bootamp_audio
    bootamp_playlist bootamp_resolve bootamp_provider bootamp_ui
    CLI11::CLI11
    ${FTXUI_COMPONENT_LIB} ${FTXUI_DOM_LIB} ${FTXUI_SCREEN_LIB})
else()
  message(STATUS "Skipping bootamp executable (needs miniaudio + ftxui + cli11)")
endif()