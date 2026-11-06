# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(BounceAESMaster VERSION 1.2.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# MSVC: static C/C++ runtime, so the service, tray and the ASIO driver (loaded into mAirList)
# run on any Windows 10/11 without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(BAM_BUILD_TESTS "Build unit tests" ON)
option(BAM_BUILD_TOOLS "Build test tools" ON)
# Bounce Encoder codecs (LAME, fdk-aac, libogg, libFLAC from pinned tarballs, cmake/codecs.cmake).
# The encoder ships 64-bit only, so a 32-bit build (the ASIO driver for 32-bit hosts) skips them.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(BAM_WITH_CODECS_DEFAULT ON)
else()
  set(BAM_WITH_CODECS_DEFAULT OFF)
endif()
option(BAM_WITH_CODECS "Fetch and build the Bounce Encoder codec libraries" ${BAM_WITH_CODECS_DEFAULT})
# Stereo Tool processing of Bounce Encoder (cmake/stereotool.cmake): the pinned Thimeo SDK for the
# build-time check of the binding and, on Windows x64, libStereoTool_64.dll next to the encoder.
# The binding itself is always built with the encoder (the library is loaded at run time).
include(CMakeDependentOption)
cmake_dependent_option(BAM_WITH_STEREOTOOL "Fetch the pinned Stereo Tool SDK (libStereoTool 11.07) for Bounce Encoder"
  ON "BAM_WITH_CODECS;CMAKE_SIZEOF_VOID_P EQUAL 8" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
  if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  endif()
endif()
if(MINGW)
  add_link_options(-static -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
endif()

find_package(Threads REQUIRED)

# ---- third party ----
add_library(bam_thirdparty INTERFACE)
target_include_directories(bam_thirdparty SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib)

# libsamplerate 0.2.2 (BSD-2-Clause), variable-ratio resampling for the Windows audio bridge
add_library(bam_samplerate STATIC
  third_party/libsamplerate/samplerate.c
  third_party/libsamplerate/src_sinc.c
  third_party/libsamplerate/src_linear.c
  third_party/libsamplerate/src_zoh.c)
target_include_directories(bam_samplerate SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libsamplerate)
target_compile_definitions(bam_samplerate PRIVATE HAVE_STDBOOL_H ENABLE_SINC_FAST_CONVERTER ENABLE_SINC_MEDIUM_CONVERTER
  PACKAGE="libsamplerate" VERSION="0.2.2")
if(MSVC)
  target_compile_options(bam_samplerate PRIVATE /W0)
else()
  target_compile_options(bam_samplerate PRIVATE -w)
  if(NOT WIN32)
    target_link_libraries(bam_samplerate PUBLIC m)
  endif()
endif()

# Bounce Encoder codecs: mp3lame, fdk-aac (shared, loaded at run time), ogg + FLAC (static)
if(BAM_WITH_CODECS)
  include(cmake/codecs.cmake)
endif()
if(BAM_WITH_STEREOTOOL)
  include(cmake/stereotool.cmake)
endif()

# ---- core ----
file(GLOB BAM_CORE_SOURCES CONFIGURE_DEPENDS core/src/*.cpp)
add_library(bam_core STATIC ${BAM_CORE_SOURCES})
target_include_directories(bam_core PUBLIC core/include)
target_link_libraries(bam_core PUBLIC bam_thirdparty bam_samplerate Threads::Threads)

# ---- platform ----
if(WIN32)
  file(GLOB BAM_PLATFORM_SOURCES CONFIGURE_DEPENDS platform/src/win/*.cpp)
else()
  file(GLOB BAM_PLATFORM_SOURCES CONFIGURE_DEPENDS platform/src/posix/*.cpp)
endif()
if(BAM_PLATFORM_SOURCES)
  add_library(bam_platform STATIC ${BAM_PLATFORM_SOURCES})
  target_include_directories(bam_platform PUBLIC platform/include)
  target_link_libraries(bam_platform PUBLIC bam_core)
  if(WIN32)
    target_link_libraries(bam_platform PUBLIC ws2_32 iphlpapi avrt winmm advapi32 shell32 ole32)
  elseif(NOT APPLE)
    target_link_libraries(bam_platform PUBLIC rt)
  endif()
endif()

# ---- panel HTTP hardening, shared by BounceAES Master and Bounce Encoder (every build) ----
file(GLOB BAM_HTTP_SOURCES CONFIGURE_DEPENDS http/src/*.cpp)
add_library(bam_http STATIC ${BAM_HTTP_SOURCES})
target_include_directories(bam_http PUBLIC http/include)
target_link_libraries(bam_http PUBLIC bam_core)
if(WIN32)
  target_link_libraries(bam_http PUBLIC ws2_32)
endif()

# ---- engine ----
file(GLOB BAM_ENGINE_SOURCES CONFIGURE_DEPENDS engine/src/*.cpp)
if(BAM_ENGINE_SOURCES)
  include(cmake/embed_files.cmake)
  add_library(bam_engine STATIC ${BAM_ENGINE_SOURCES} ${BAM_PANEL_ASSETS_CPP})
  target_include_directories(bam_engine PUBLIC engine/include)
  target_link_libraries(bam_engine PUBLIC bam_platform bam_http)
endif()

# ---- Bounce Encoder: audio encoders (64-bit builds with the codecs) ----
if(BAM_WITH_CODECS)
  # LAME and fdk-aac headers only, for the run-time loaded function types and constants
  add_library(bam_codec_headers INTERFACE)
  target_include_directories(bam_codec_headers SYSTEM INTERFACE
    $<TARGET_PROPERTY:mp3lame,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:fdk-aac,INTERFACE_INCLUDE_DIRECTORIES>)
  file(GLOB BAM_ENCODER_SOURCES CONFIGURE_DEPENDS encoder/src/*.cpp)
  # Bounce Encoder panel: encoder/panel/ and panel-common/ -> bam::encoder::kEncoderPanelAssets
  include(cmake/embed_files.cmake)
  set(BAM_ENCODER_PANEL_ASSETS_CPP ${CMAKE_CURRENT_BINARY_DIR}/encoder_panel_assets.cpp)
  bam_embed_files(encoder/panel ${BAM_ENCODER_PANEL_ASSETS_CPP} "bam/encoder/http_server.hpp" bam::encoder
    EncoderPanelAsset kEncoderPanelAssets kEncoderPanelAssetCount panel-common)
  add_library(bam_encoder STATIC ${BAM_ENCODER_SOURCES} ${BAM_ENCODER_PANEL_ASSETS_CPP})
  target_include_directories(bam_encoder PUBLIC encoder/include)
  target_link_libraries(bam_encoder PUBLIC bam_platform bam_http PRIVATE bam_codec_headers FLAC::FLAC)
  # the shared codecs are loaded at run time; build them with anything that uses the encoder
  add_dependencies(bam_encoder mp3lame fdk-aac)
  if(BAM_WITH_STEREOTOOL)
    add_dependencies(bam_encoder bam_stereotool_sdk_check)  # the binding matches the SDK
  endif()
endif()

# ---- service ----
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/service/main.cpp)
  set(BAM_SERVICE_SOURCES service/main.cpp)
  if(WIN32)
    list(APPEND BAM_SERVICE_SOURCES service/win_service.cpp service/service.rc)
  endif()
  add_executable(BounceAESService ${BAM_SERVICE_SOURCES})
  target_link_libraries(BounceAESService PRIVATE bam_engine)
endif()

# ---- Bounce Encoder service (64-bit builds with the codecs) ----
if(TARGET bam_encoder)
  set(BAM_ENCODER_SERVICE_SOURCES encoder_service/main.cpp)
  if(WIN32)
    list(APPEND BAM_ENCODER_SERVICE_SOURCES service/win_service.cpp encoder_service/encoder.rc)
  endif()
  add_executable(BounceEncoder ${BAM_ENCODER_SERVICE_SOURCES})
  target_include_directories(BounceEncoder PRIVATE service)
  target_link_libraries(BounceEncoder PRIVATE bam_encoder)
  # the run-time codec libraries go next to the executable (set_codec_library_dirs default)
  add_dependencies(BounceEncoder mp3lame fdk-aac)
  if(BAM_STEREOTOOL_DLL)
    # and so does Stereo Tool (Windows x64), where the encoder loads it from
    add_custom_target(bam_stereotool_dll ALL
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:BounceEncoder>"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BAM_STEREOTOOL_DLL}" "$<TARGET_FILE_DIR:BounceEncoder>/libStereoTool_64.dll"
      VERBATIM)
  endif()
endif()

# ---- Windows-only apps ----
if(WIN32)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/asio/asio_driver.cpp)
    add_library(BounceAESAsio SHARED asio/asio_driver.cpp asio/dll_main.cpp asio/BounceAESAsio.def)
    target_include_directories(BounceAESAsio PRIVATE third_party/asiosdk)
    target_link_libraries(BounceAESAsio PRIVATE bam_platform ole32 uuid shell32 winmm avrt advapi32)
    set_target_properties(BounceAESAsio PROPERTIES PREFIX "")
    if(MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 4)
      # 32-bit: the .def exports the undecorated COM names of the stdcall functions
      target_link_options(BounceAESAsio PRIVATE -Wl,--enable-stdcall-fixup)
    endif()
  endif()
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bridge/bridge_main.cpp)
    add_executable(BounceAESAudioBridge WIN32 bridge/bridge_main.cpp bridge/wasapi_stream.cpp bridge/endpoints.cpp bridge/bridge.rc)
    target_link_libraries(BounceAESAudioBridge PRIVATE bam_platform ole32 uuid avrt mmdevapi propsys)
    if(MINGW)
      target_link_options(BounceAESAudioBridge PRIVATE -municode)
    endif()
  endif()
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tray/tray_main.cpp)
    add_executable(BounceAESTray WIN32 tray/tray_main.cpp tray/tray.rc)
    target_link_libraries(BounceAESTray PRIVATE bam_core winhttp shell32 user32 gdi32 comctl32)
    if(MSVC)
      target_sources(BounceAESTray PRIVATE tray/tray.manifest)  # merged by the linker (/MANIFESTINPUT)
    endif()
    if(MINGW)
      target_link_options(BounceAESTray PRIVATE -municode)
    endif()
  endif()
  if(TARGET bam_encoder)
    add_executable(BounceEncoderTray WIN32 encoder_tray/encoder_tray_main.cpp encoder_tray/encoder_tray.rc)
    target_include_directories(BounceEncoderTray PRIVATE encoder/include)  # constants only, nothing linked
    target_link_libraries(BounceEncoderTray PRIVATE bam_core winhttp shell32 user32 gdi32 comctl32 ole32)
    if(MSVC)
      target_sources(BounceEncoderTray PRIVATE encoder_tray/encoder_tray.manifest)  # merged by the linker (/MANIFESTINPUT)
    endif()
    if(MINGW)
      target_link_options(BounceEncoderTray PRIVATE -municode)
    endif()
  endif()
endif()

# ---- tools ----
if(BAM_BUILD_TOOLS AND WIN32)
  # what an ASIO host sees: build in both bitnesses (64-bit and 32-bit hosts read different registries)
  add_executable(asio-probe tools/asio_probe.cpp)
  target_include_directories(asio-probe PRIVATE third_party/asiosdk)
  target_link_libraries(asio-probe PRIVATE ole32 advapi32 user32)
  if(MINGW)
    target_link_options(asio-probe PRIVATE -municode)
  endif()
endif()
if(BAM_BUILD_TOOLS AND TARGET bam_engine)
  foreach(tool bounceaes_test ptp_sim)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/${tool}.cpp)
      string(REPLACE "_" "-" tool_name ${tool})
      add_executable(${tool_name} tools/${tool}.cpp)
      target_link_libraries(${tool_name} PRIVATE bam_engine)
    endif()
  endforeach()
endif()

# ---- tests ----
if(BAM_BUILD_TESTS)
  enable_testing()
  file(GLOB BAM_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
  add_executable(bam_tests ${BAM_TEST_SOURCES})
  if(TARGET bam_engine)
    target_link_libraries(bam_tests PRIVATE bam_engine)
  elseif(TARGET bam_platform)
    target_link_libraries(bam_tests PRIVATE bam_platform bam_http)
  else()
    target_link_libraries(bam_tests PRIVATE bam_core bam_http)
  endif()
  target_compile_definitions(bam_tests PRIVATE BAM_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  if(BAM_WITH_CODECS)
    # the shared codecs are loaded at run time from where the build put them
    target_compile_definitions(bam_tests PRIVATE BAM_WITH_CODECS
      BAM_MP3LAME_PATH="$<TARGET_FILE:mp3lame>" BAM_FDKAAC_PATH="$<TARGET_FILE:fdk-aac>")
    target_link_libraries(bam_tests PRIVATE bam_encoder bam_codec_headers FLAC::FLAC)
    add_dependencies(bam_tests mp3lame fdk-aac)
  endif()
  if(TARGET bam_encoder)
    # A stand-in for libStereoTool under the real library's file name, in a folder of its own
    # (never next to the executables, where the encoder looks for the real one); the tests point
    # the binding at that folder.
    add_library(fake_stereotool SHARED tests/fake_stereotool/fake_stereotool.cpp)
    if(WIN32)
      set(_bam_fake_st_name StereoTool_64)
    elseif(APPLE)
      set(_bam_fake_st_name StereoTool_64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
      set(_bam_fake_st_name StereoTool_noX11_arm64)
    else()
      set(_bam_fake_st_name StereoTool_intel64)
    endif()
    set_target_properties(fake_stereotool PROPERTIES
      PREFIX lib OUTPUT_NAME ${_bam_fake_st_name}
      RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/fake_stereotool
      LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/fake_stereotool
      ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/fake_stereotool)
    target_compile_definitions(bam_tests PRIVATE
      BAM_FAKE_STEREOTOOL_PATH="$<TARGET_FILE:fake_stereotool>"
      BAM_FAKE_STEREOTOOL_DIR="$<TARGET_FILE_DIR:fake_stereotool>")
    add_dependencies(bam_tests fake_stereotool)
  endif()
  add_test(NAME bam_tests COMMAND bam_tests)
  # a hanging test must not hold ctest (or CI) for ever; with the progress output of
  # tests/test_main.cpp a timeout names the test that hung
  set_tests_properties(bam_tests PROPERTIES TIMEOUT 1200 ENVIRONMENT BAM_TEST_PROGRESS=1)
  # the Bounce Encoder panel's pure helpers (encoder/panel/logic.js), with node's built-in test runner
  find_program(BAM_NODE node)
  if(BAM_NODE)
    add_test(NAME encoder_panel_logic COMMAND ${BAM_NODE} --test ${CMAKE_CURRENT_SOURCE_DIR}/tests/panel/encoder_panel_logic.test.js)
    set_tests_properties(encoder_panel_logic PROPERTIES TIMEOUT 120)
  endif()
endif()
