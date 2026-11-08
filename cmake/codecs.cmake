# SPDX-License-Identifier: GPL-3.0-or-later
# Codec libraries for Bounce Encoder, built from pinned source tarballs on every platform
# (macOS/Linux host, MinGW cross builds, MSVC in CI), so all builds use the same versions.
#
#   LAME 3.100    (LGPL)          -> shared  mp3lame  (libmp3lame.dll), own CMake in cmake/lame
#   fdk-aac 2.0.3 (Fraunhofer)    -> shared  fdk-aac  (libfdk-aac-2.dll)
#   libogg 1.3.5  (BSD-3-Clause)  -> static  ogg      (Ogg::ogg)
#   flac 1.4.3    (BSD-3-Clause)  -> static  FLAC     (FLAC::FLAC)
#
# LAME and fdk-aac stay separate shared libraries loaded at run time (spec §5.3); they are
# written next to the executables (BAM_CODEC_OUTPUT_DIR) so run-time loading finds them.
# Third-party code is configured without our warning flags and with its own warnings off.
include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/download.cmake)

set(BAM_CODEC_OUTPUT_DIR ${PROJECT_BINARY_DIR})

# BAM_CODEC_CACHE: a folder for the source tarballs (CI keeps it in its cache, so a download outage
# at sourceforge or xiph.org does not break the build): a tarball found there (checked against its
# SHA-256) is used without any download, a missing one is downloaded into it first. Relative to the
# source directory. Empty (the default): FetchContent downloads them itself. Either way, configuring
# with the codecs needs network access the first time (or -DBAM_WITH_CODECS=OFF).
set(BAM_CODEC_CACHE "" CACHE STRING "Folder for the codec source tarballs (relative to the source directory)")

# One codec source: SOURCE_SUBDIR points nowhere, FetchContent only downloads and unpacks, the
# subdirectories are added below (EXCLUDE_FROM_ALL, with their options scoped to this file).
# The first URL is the project's own; the others are mirrors of the same file (the same pinned
# SHA-256 is required of each), tried when it fails (cmake/download.cmake: retries, then the next).
function(bam_codec_source name sha256 url)
  set(urls ${url} ${ARGN})
  if(BAM_CODEC_CACHE)
    get_filename_component(cache "${BAM_CODEC_CACHE}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    get_filename_component(file "${url}" NAME)
    set(local "${cache}/${name}-${file}")
    if(EXISTS "${local}")
      file(SHA256 "${local}" have)
      if(NOT have STREQUAL sha256)
        message(STATUS "${local}: wrong SHA-256, downloading again")
        file(REMOVE "${local}")
      endif()
    endif()
    if(NOT EXISTS "${local}")
      file(MAKE_DIRECTORY "${cache}")
      bam_download_pinned("${local}" ${sha256} error wrong ${urls})
      if(error)
        message(FATAL_ERROR "Cannot download ${name} (${urls}): ${error} (configure with -DBAM_WITH_CODECS=OFF to build without the codecs)")
      endif()
    endif()
    set(urls "${local}")
  endif()
  FetchContent_Declare(${name} URL ${urls} URL_HASH SHA256=${sha256} SOURCE_SUBDIR bam-no-cmake)
endfunction()

bam_codec_source(bam_lame ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
  https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
  https://sourceforge.net/projects/lame/files/lame/3.100/lame-3.100.tar.gz/download
  https://deb.debian.org/debian/pool/main/l/lame/lame_3.100.orig.tar.gz)
bam_codec_source(bam_fdkaac e25671cd96b10bad896aa42ab91a695a9e573395262baed4e4a2ff178d6a3a78
  https://github.com/mstorsjo/fdk-aac/archive/refs/tags/v2.0.3.tar.gz
  https://codeload.github.com/mstorsjo/fdk-aac/tar.gz/refs/tags/v2.0.3)
bam_codec_source(bam_ogg 0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664
  https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.gz
  https://ftp.osuosl.org/pub/xiph/releases/ogg/libogg-1.3.5.tar.gz
  https://github.com/xiph/ogg/releases/download/v1.3.5/libogg-1.3.5.tar.gz)
bam_codec_source(bam_flac 6c58e69cd22348f441b861092b825e591d0b822e106de6eb0ee4d05d27205b70
  https://downloads.xiph.org/releases/flac/flac-1.4.3.tar.xz
  https://ftp.osuosl.org/pub/xiph/releases/flac/flac-1.4.3.tar.xz
  https://github.com/xiph/flac/releases/download/1.4.3/flac-1.4.3.tar.xz)
FetchContent_MakeAvailable(bam_lame bam_fdkaac bam_ogg bam_flac)

function(bam_silence_third_party)
  foreach(target IN LISTS ARGN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /W0)
    else()
      target_compile_options(${target} PRIVATE -w)
    endif()
  endforeach()
endfunction()

# Variables set here stay in this function's scope, i.e. only the codec subprojects see them.
function(bam_add_codec_subprojects)
  # Our warning flags and Windows defines are not for third-party code: hide this directory's
  # compile options/definitions while the subdirectories copy them, restore them afterwards.
  get_directory_property(saved_options COMPILE_OPTIONS)
  get_directory_property(saved_definitions COMPILE_DEFINITIONS)
  set_directory_properties(PROPERTIES COMPILE_OPTIONS "" COMPILE_DEFINITIONS "")

  # Old cmake_minimum_required() in the subprojects (libogg asks for 2.8.12, removed in CMake 4):
  # treat them as 3.10, and let option() and CMAKE_MSVC_RUNTIME_LIBRARY (static CRT) apply.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

  set(BUILD_TESTING OFF)
  set(BUILD_PROGRAMS OFF)
  set(BUILD_EXAMPLES OFF)
  set(BUILD_DOCS OFF)

  # LAME: shared, own CMake
  set(LAME_SOURCE_DIR ${bam_lame_SOURCE_DIR})
  add_subdirectory(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/lame ${bam_lame_BINARY_DIR} EXCLUDE_FROM_ALL)

  # fdk-aac: shared (its own CMake), Windows name libfdk-aac-2.dll for MSVC as well as MinGW
  set(BUILD_SHARED_LIBS ON)
  set(FDK_AAC_INSTALL_CMAKE_CONFIG_MODULE OFF)
  set(FDK_AAC_INSTALL_PKGCONFIG_MODULE OFF)
  add_subdirectory(${bam_fdkaac_SOURCE_DIR} ${bam_fdkaac_BINARY_DIR} EXCLUDE_FROM_ALL)
  set_target_properties(fdk-aac PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${BAM_CODEC_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${BAM_CODEC_OUTPUT_DIR})
  if(WIN32)
    set_target_properties(fdk-aac PROPERTIES PREFIX lib RUNTIME_OUTPUT_NAME fdk-aac-2)
  endif()

  # libogg + libFLAC: static, linked into the encoder
  set(BUILD_SHARED_LIBS OFF)
  set(INSTALL_DOCS OFF)
  set(INSTALL_PKG_CONFIG_MODULE OFF)
  set(INSTALL_CMAKE_PACKAGE_MODULE OFF)
  add_subdirectory(${bam_ogg_SOURCE_DIR} ${bam_ogg_BINARY_DIR} EXCLUDE_FROM_ALL)

  set(WITH_OGG ON)
  set(BUILD_CXXLIBS OFF)
  set(BUILD_UTILS OFF)
  set(INSTALL_MANPAGES OFF)
  set(INSTALL_PKGCONFIG_MODULES OFF)
  set(INSTALL_CMAKE_CONFIG_MODULE OFF)
  if(MINGW)
    # would pull libssp into everything that links libFLAC
    set(WITH_STACK_PROTECTOR OFF)
    set(WITH_FORTIFY_SOURCE OFF)
  endif()
  add_subdirectory(${bam_flac_SOURCE_DIR} ${bam_flac_BINARY_DIR} EXCLUDE_FROM_ALL)

  bam_silence_third_party(fdk-aac ogg FLAC)  # mp3lame: cmake/lame does it

  set_directory_properties(PROPERTIES COMPILE_OPTIONS "${saved_options}" COMPILE_DEFINITIONS "${saved_definitions}")
endfunction()
bam_add_codec_subprojects()

# The shared codec libraries are part of the default build (the subdirectories are
# EXCLUDE_FROM_ALL, so nothing else of the subprojects is built).
add_custom_target(bam_codec_libs ALL)
add_dependencies(bam_codec_libs mp3lame fdk-aac)

# Licence texts of the code that ships in Bounce Encoder binaries (LGPL for LAME, the FDK AAC
# licence, the BSD notices), next to the codec libraries in encoder-licenses/ for the installer
# and the CI artifact. $<TARGET_FILE_DIR> also covers the per-configuration folder of MSVC.
set(BAM_ENCODER_LICENSES
  "${bam_lame_SOURCE_DIR}/COPYING|LAME-COPYING.txt"
  "${bam_fdkaac_SOURCE_DIR}/NOTICE|fdk-aac-NOTICE.txt"
  "${bam_ogg_SOURCE_DIR}/COPYING|libogg-COPYING.txt"
  "${bam_flac_SOURCE_DIR}/COPYING.Xiph|flac-COPYING.Xiph.txt"
  "${PROJECT_SOURCE_DIR}/third_party/libsamplerate/COPYING|libsamplerate-COPYING.txt")
set(_bam_license_commands COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mp3lame>/encoder-licenses")
foreach(entry IN LISTS BAM_ENCODER_LICENSES)
  string(REPLACE "|" ";" entry "${entry}")
  list(GET entry 0 _src)
  list(GET entry 1 _dst)
  list(APPEND _bam_license_commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "$<TARGET_FILE_DIR:mp3lame>/encoder-licenses/${_dst}")
endforeach()
add_custom_command(TARGET bam_codec_libs POST_BUILD ${_bam_license_commands} VERBATIM)
