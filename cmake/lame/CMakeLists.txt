# SPDX-License-Identifier: GPL-3.0-or-later
# LAME (LGPL-2.0-or-later) has no CMake build: libmp3lame + mpglib as one shared library
# (libmp3lame.dll / libmp3lame.dylib / libmp3lame.so), loaded at run time (spec §5.3).
# Expects LAME_SOURCE_DIR (the unpacked lame-3.100 tarball) and BAM_CODEC_OUTPUT_DIR.
set(LAME_VERSION 3.100)

set(LAME_LIB_SOURCES
  VbrTag.c bitstream.c encoder.c fft.c gain_analysis.c id3tag.c lame.c newmdct.c presets.c
  psymodel.c quantize.c quantize_pvt.c reservoir.c set_get.c tables.c takehiro.c util.c
  vbrquantize.c version.c mpglib_interface.c)
list(TRANSFORM LAME_LIB_SOURCES PREPEND ${LAME_SOURCE_DIR}/libmp3lame/)
set(LAME_MPGLIB_SOURCES
  common.c dct64_i386.c decode_i386.c interface.c layer1.c layer2.c layer3.c tabinit.c)
list(TRANSFORM LAME_MPGLIB_SOURCES PREPEND ${LAME_SOURCE_DIR}/mpglib/)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h @ONLY)

# Exported API: the upstream symbol list (include/libmp3lame.sym), as each linker wants it.
file(STRINGS ${LAME_SOURCE_DIR}/include/libmp3lame.sym LAME_SYMBOLS REGEX "^[A-Za-z_][A-Za-z0-9_]*$")
# listed upstream but static in 3.100 (DEPRECATED_OR_OBSOLETE_CODE_REMOVED); distributions drop it too
list(REMOVE_ITEM LAME_SYMBOLS lame_init_old)
set(LAME_EXPORTS_FILE "")
if(WIN32)
  list(JOIN LAME_SYMBOLS "\n  " _lame_lines)
  set(LAME_EXPORTS_FILE ${CMAKE_CURRENT_BINARY_DIR}/libmp3lame.def)
  file(CONFIGURE OUTPUT ${LAME_EXPORTS_FILE} CONTENT "EXPORTS\n  ${_lame_lines}\n" @ONLY)
elseif(APPLE)
  list(TRANSFORM LAME_SYMBOLS PREPEND _ OUTPUT_VARIABLE _lame_mach)
  list(JOIN _lame_mach "\n" _lame_lines)
  set(LAME_EXPORTS_FILE ${CMAKE_CURRENT_BINARY_DIR}/libmp3lame.exp)
  file(CONFIGURE OUTPUT ${LAME_EXPORTS_FILE} CONTENT "${_lame_lines}\n" @ONLY)
else()
  list(JOIN LAME_SYMBOLS ";\n    " _lame_lines)
  set(LAME_EXPORTS_FILE ${CMAKE_CURRENT_BINARY_DIR}/libmp3lame.ver)
  file(CONFIGURE OUTPUT ${LAME_EXPORTS_FILE} CONTENT "{\n  global:\n    ${_lame_lines};\n  local: *;\n};\n" @ONLY)
endif()

add_library(mp3lame SHARED ${LAME_LIB_SOURCES} ${LAME_MPGLIB_SOURCES})
target_include_directories(mp3lame
  PUBLIC ${LAME_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${LAME_SOURCE_DIR}/libmp3lame ${LAME_SOURCE_DIR}/mpglib)
target_compile_definitions(mp3lame PRIVATE HAVE_CONFIG_H $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
set_target_properties(mp3lame PROPERTIES
  OUTPUT_NAME mp3lame
  PREFIX lib
  C_VISIBILITY_PRESET default
  RUNTIME_OUTPUT_DIRECTORY ${BAM_CODEC_OUTPUT_DIR}
  LIBRARY_OUTPUT_DIRECTORY ${BAM_CODEC_OUTPUT_DIR}
  LINK_DEPENDS ${LAME_EXPORTS_FILE})
if(WIN32)
  target_sources(mp3lame PRIVATE ${LAME_EXPORTS_FILE})
elseif(APPLE)
  target_link_options(mp3lame PRIVATE "LINKER:-exported_symbols_list,${LAME_EXPORTS_FILE}")
else()
  target_link_options(mp3lame PRIVATE "LINKER:--version-script=${LAME_EXPORTS_FILE}")
  target_link_libraries(mp3lame PRIVATE m)
endif()
# third-party code: its warnings are not ours
if(MSVC)
  target_compile_options(mp3lame PRIVATE /W0)
else()
  target_compile_options(mp3lame PRIVATE -w)
endif()
