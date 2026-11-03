# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(BounceAESMaster VERSION 1.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# MSVC: static C/C++ runtime, so the service, tray and the ASIO driver (loaded into mAirList)
# run on any Windows 10/11 without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(BAM_BUILD_TESTS "Build unit tests" ON)
option(BAM_BUILD_TOOLS "Build test tools" ON)

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

# ---- engine ----
file(GLOB BAM_ENGINE_SOURCES CONFIGURE_DEPENDS engine/src/*.cpp)
if(BAM_ENGINE_SOURCES)
  include(cmake/embed_files.cmake)
  add_library(bam_engine STATIC ${BAM_ENGINE_SOURCES} ${BAM_PANEL_ASSETS_CPP})
  target_include_directories(bam_engine PUBLIC engine/include)
  target_link_libraries(bam_engine PUBLIC bam_platform)
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
    target_link_libraries(bam_tests PRIVATE bam_platform)
  else()
    target_link_libraries(bam_tests PRIVATE bam_core)
  endif()
  target_compile_definitions(bam_tests PRIVATE BAM_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  add_test(NAME bam_tests COMMAND bam_tests)
endif()
