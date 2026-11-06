# SPDX-License-Identifier: GPL-3.0-or-later
# Thimeo Stereo Tool SDK (libStereoTool 11.07; commercial, not GPL) for Bounce Encoder 1.1.
# The zip is pinned by its SHA-256, and so is every file taken from it:
#   include/libStereoTool.h, include/ParameterEnum.h
#       only for the build-time check of the binding's own declarations (cmake/stereotool/
#       sdk_check.cpp against encoder/src/stereotool_api.hpp); the encoder never includes them
#       and they are not part of the repository
#   lib/Windows/64/libStereoTool_64.dll (Windows x64 builds only)
#       copied next to BounceEncoder.exe, where the encoder loads it from at run time
#
# Where the zip comes from (first match):
#   BAM_STEREOTOOL_SDK_ZIP  a local copy of the zip (checked against the SHA-256, nothing downloaded)
#   BAM_CODEC_CACHE         the codec download cache (CI): used when there, else downloaded into it
#   otherwise               downloaded into this build folder (_deps/), once
# The extracted files are checked against their own SHA-256 on every configure; while they are
# present and correct the zip is not opened again.

set(BAM_STEREOTOOL_SDK_URL "https://download.thimeo.com/Stereo_Tool_Generic_plugin.zip")
set(BAM_STEREOTOOL_SDK_SHA256 ed07c086cd95523abf5e7d655fa1f32e556fca2539ff0e85b4e12ff3a4c3ec22)
set(BAM_STEREOTOOL_SDK_ZIP "" CACHE FILEPATH "Local copy of Stereo_Tool_Generic_plugin.zip (pinned SHA-256; empty: download)")

set(_bam_st_dir "${PROJECT_BINARY_DIR}/_deps/stereotool-sdk/libStereoTool_1107")
set(_bam_st_files
  "include/libStereoTool.h|b6581abfa420dd5899bc591b6414d9434953f6894acfd21dea3dbb5f232c1e31"
  "include/ParameterEnum.h|163386c9b297148ff8392f27ea852acc32c8987340c5192cb7d14c18b96e214a")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  list(APPEND _bam_st_files
    "lib/Windows/64/libStereoTool_64.dll|48cbdc06f44a203f7fa2895b3ec79fc63557d61dcd687563c6a17388d6308c4e")
endif()

# TRUE when every file is extracted and has its SHA-256.
function(_bam_st_files_ok out)
  foreach(entry IN LISTS _bam_st_files)
    string(REPLACE "|" ";" entry "${entry}")
    list(GET entry 0 rel)
    list(GET entry 1 sha)
    if(NOT EXISTS "${_bam_st_dir}/${rel}")
      set(${out} FALSE PARENT_SCOPE)
      return()
    endif()
    file(SHA256 "${_bam_st_dir}/${rel}" have)
    if(NOT have STREQUAL sha)
      message(STATUS "Stereo Tool SDK: ${rel} has a wrong SHA-256")
      set(${out} FALSE PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out} TRUE PARENT_SCOPE)
endfunction()

# The path of the verified zip.
function(_bam_st_zip out)
  set(sha ${BAM_STEREOTOOL_SDK_SHA256})
  if(BAM_STEREOTOOL_SDK_ZIP)
    get_filename_component(zip "${BAM_STEREOTOOL_SDK_ZIP}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    if(NOT EXISTS "${zip}")
      message(FATAL_ERROR "BAM_STEREOTOOL_SDK_ZIP: ${zip} does not exist")
    endif()
    file(SHA256 "${zip}" have)
    if(NOT have STREQUAL sha)
      message(FATAL_ERROR "BAM_STEREOTOOL_SDK_ZIP: ${zip} has SHA-256 ${have}, expected ${sha}")
    endif()
    set(${out} "${zip}" PARENT_SCOPE)
    return()
  endif()
  if(BAM_CODEC_CACHE)
    get_filename_component(cache "${BAM_CODEC_CACHE}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
  else()
    set(cache "${PROJECT_BINARY_DIR}/_deps")
  endif()
  set(zip "${cache}/bam_stereotool-Stereo_Tool_Generic_plugin.zip")
  if(EXISTS "${zip}")
    file(SHA256 "${zip}" have)
    if(NOT have STREQUAL sha)
      message(STATUS "${zip}: wrong SHA-256, downloading again")
      file(REMOVE "${zip}")
    endif()
  endif()
  if(NOT EXISTS "${zip}")
    message(STATUS "Downloading the Stereo Tool SDK (about 360 MB): ${BAM_STEREOTOOL_SDK_URL}")
    file(MAKE_DIRECTORY "${cache}")
    file(DOWNLOAD "${BAM_STEREOTOOL_SDK_URL}" "${zip}.part" EXPECTED_HASH SHA256=${sha} STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      file(REMOVE "${zip}.part")
      message(FATAL_ERROR "Cannot download ${BAM_STEREOTOOL_SDK_URL}: ${status} (configure with -DBAM_WITH_STEREOTOOL=OFF to build without Stereo Tool)")
    endif()
    file(RENAME "${zip}.part" "${zip}")
  endif()
  set(${out} "${zip}" PARENT_SCOPE)
endfunction()

_bam_st_files_ok(_bam_st_ok)
if(NOT _bam_st_ok)
  _bam_st_zip(_bam_st_zip_path)
  # The zip's member names use backslashes; the extraction turns them into folders. Only the
  # files listed above are extracted.
  set(_bam_st_patterns "")
  foreach(entry IN LISTS _bam_st_files)
    string(REPLACE "|" ";" entry "${entry}")
    list(GET entry 0 rel)
    string(REPLACE "/" "*" pattern "*${rel}")
    list(APPEND _bam_st_patterns "${pattern}")
  endforeach()
  file(REMOVE_RECURSE "${PROJECT_BINARY_DIR}/_deps/stereotool-sdk")
  file(ARCHIVE_EXTRACT INPUT "${_bam_st_zip_path}" DESTINATION "${PROJECT_BINARY_DIR}/_deps/stereotool-sdk"
    PATTERNS ${_bam_st_patterns})
  _bam_st_files_ok(_bam_st_ok)
  if(NOT _bam_st_ok)
    message(FATAL_ERROR "Stereo Tool SDK: the files extracted from ${_bam_st_zip_path} do not match their pinned SHA-256")
  endif()
endif()

set(BAM_STEREOTOOL_INCLUDE_DIR "${_bam_st_dir}/include")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(BAM_STEREOTOOL_DLL "${_bam_st_dir}/lib/Windows/64/libStereoTool_64.dll")
else()
  set(BAM_STEREOTOOL_DLL "")
endif()

# The binding declares the library's functions itself; this object library (part of every build
# with the SDK) fails to compile when a declaration or a parameter index does not match the SDK.
add_library(bam_stereotool_sdk_check OBJECT ${CMAKE_CURRENT_LIST_DIR}/stereotool/sdk_check.cpp)
target_include_directories(bam_stereotool_sdk_check PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../encoder/src)
target_include_directories(bam_stereotool_sdk_check SYSTEM PRIVATE ${BAM_STEREOTOOL_INCLUDE_DIR})
if(APPLE)
  target_compile_definitions(bam_stereotool_sdk_check PRIVATE _ST_MAC)
elseif(NOT WIN32)
  target_compile_definitions(bam_stereotool_sdk_check PRIVATE _ST_LINUX)
endif()
