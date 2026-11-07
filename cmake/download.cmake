# SPDX-License-Identifier: GPL-3.0-or-later
# A pinned download for the configure step (cmake/codecs.cmake, cmake/stereotool.cmake): the URLs
# are tried in order, in up to three rounds (2 s and then 5 s apart), until one gives a file with
# the expected SHA-256; only that file is kept. A URL that answers with other content (a wrong
# SHA-256) is not asked again: that is no outage. One server's outage or TLS trouble (CI saw
# "SSL connect error" from downloads.xiph.org on macOS) then falls back to the next URL.
#
#   bam_download_pinned(<file> <sha256> <error-var> <wrong-sha-var> <url>...)
#     <error-var>      empty on success, else what went wrong with the last URL tried
#     <wrong-sha-var>  the SHA-256 a URL delivered instead of <sha256> (empty when none did)
include_guard(GLOBAL)

function(bam_download_pinned file sha error_var wrong_var)
  set(urls ${ARGN})
  set(error "no URL")
  set(wrong "")
  set(waits 2 5)
  foreach(round RANGE 2)
    if(NOT urls)
      break()
    endif()
    set(next_urls "")
    foreach(url IN LISTS urls)
      file(REMOVE "${file}.part")
      file(DOWNLOAD "${url}" "${file}.part" STATUS status TLS_VERIFY ON)
      list(GET status 0 code)
      if(code EQUAL 0)
        file(SHA256 "${file}.part" have)
        if(have STREQUAL sha)
          file(RENAME "${file}.part" "${file}")
          set(${error_var} "" PARENT_SCOPE)
          set(${wrong_var} "" PARENT_SCOPE)
          return()
        endif()
        set(wrong "${have}")
        set(error "${url}: SHA-256 ${have}, expected ${sha}")
      else()
        set(error "${url}: ${status}")
        list(APPEND next_urls "${url}")  # an outage: asked again in the next round
      endif()
      file(REMOVE "${file}.part")
      message(STATUS "Download failed (${error})")
    endforeach()
    set(urls ${next_urls})
    if(urls AND round LESS 2)
      list(GET waits ${round} wait)
      message(STATUS "Trying again in ${wait} s")
      execute_process(COMMAND ${CMAKE_COMMAND} -E sleep ${wait})
    endif()
  endforeach()
  set(${error_var} "${error}" PARENT_SCOPE)
  set(${wrong_var} "${wrong}" PARENT_SCOPE)
endfunction()
