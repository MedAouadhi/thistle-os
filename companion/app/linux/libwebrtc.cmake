# libwebrtc for flutter_webrtc, pinned by hash (ADR-0016).
#
# flutter_webrtc's Linux build downloads a prebuilt libwebrtc from
# github.com/webrtc-sdk/libwebrtc into its own third_party/ folder in the
# pub cache, and checks nothing. Included before the plugins, this fetches
# the release named here, checks it against the SHA-256 below, and unpacks
# it where the plugin looks, so the plugin downloads nothing itself. A zip
# already there with another hash is replaced; a download with another
# hash fails the build. The same runs locally and in CI, from `flutter
# build linux`.
#
# Moving flutter_webrtc to a version that asks for another release fails
# here until LIBWEBRTC_RELEASE and LIBWEBRTC_SHA256 follow it: download the
# new zip, read its hash with sha256sum, and run ticket 02's freeze check
# (.scratch/tailnet-video/issues/02-*) again.

set(LIBWEBRTC_RELEASE "libwebrtc.m150.7871.07")
set(LIBWEBRTC_ASSET "libwebrtc-linux-x64-release")
set(LIBWEBRTC_SHA256 "ef380526cf9616848bc0b97b24990cbf2ad851c209d5dce93cc3ebb9d3789fbc")
set(LIBWEBRTC_URL
  "https://github.com/webrtc-sdk/libwebrtc/releases/download/${LIBWEBRTC_RELEASE}/${LIBWEBRTC_ASSET}.zip")

if((DEFINED FLUTTER_TARGET_PLATFORM AND FLUTTER_TARGET_PLATFORM MATCHES "arm64")
   OR (NOT DEFINED FLUTTER_TARGET_PLATFORM AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64"))
  message(FATAL_ERROR "libwebrtc: only linux-x64 is pinned (companion/app/linux/libwebrtc.cmake)")
endif()

set(_plugin "${CMAKE_CURRENT_SOURCE_DIR}/flutter/ephemeral/.plugin_symlinks/flutter_webrtc/third_party")
if(NOT EXISTS "${_plugin}/libwebrtc_version.ini")
  message(FATAL_ERROR "libwebrtc: flutter_webrtc not found at ${_plugin}; run flutter pub get")
endif()
get_filename_component(_third "${_plugin}" REALPATH)

# The plugin must ask for the release pinned here.
file(STRINGS "${_third}/libwebrtc_version.ini" _asked REGEX "^binary_version[ \t]*=")
string(REGEX REPLACE "^binary_version[ \t]*=[ \t]*" "" _asked "${_asked}")
string(STRIP "${_asked}" _asked)
if(NOT _asked STREQUAL LIBWEBRTC_RELEASE)
  message(FATAL_ERROR "libwebrtc: flutter_webrtc asks for ${_asked}, but ${LIBWEBRTC_RELEASE} is pinned "
                      "(companion/app/linux/libwebrtc.cmake): pin the new release with its SHA-256")
endif()

set(_zip "${_third}/downloads/${LIBWEBRTC_ASSET}.zip")
if(EXISTS "${_zip}")
  file(SHA256 "${_zip}" _have)
  if(NOT _have STREQUAL LIBWEBRTC_SHA256)
    message(WARNING "libwebrtc: ${_zip} has SHA-256 ${_have}, not the pinned one; fetching it again")
    file(REMOVE "${_zip}")
  endif()
endif()
if(NOT EXISTS "${_zip}")
  message(STATUS "libwebrtc: fetching ${LIBWEBRTC_URL}")
  file(MAKE_DIRECTORY "${_third}/downloads")
  file(DOWNLOAD "${LIBWEBRTC_URL}" "${_zip}.part" STATUS _status EXPECTED_HASH SHA256=${LIBWEBRTC_SHA256})
  list(GET _status 0 _code)
  if(NOT _code EQUAL 0)
    file(REMOVE "${_zip}.part")
    message(FATAL_ERROR "libwebrtc: ${LIBWEBRTC_URL} did not download with the pinned SHA-256: ${_status}")
  endif()
  file(RENAME "${_zip}.part" "${_zip}")
endif()

# What the plugin links must come from that zip: a stamp says which zip
# the unpacked folder came from, and anything else is unpacked again.
set(_stamp "${_third}/libwebrtc/.sha256")
set(_unpacked "")
if(EXISTS "${_stamp}")
  file(READ "${_stamp}" _unpacked)
  string(STRIP "${_unpacked}" _unpacked)
endif()
if(NOT _unpacked STREQUAL LIBWEBRTC_SHA256)
  set(_staging "${_third}/.libwebrtc_pinned")
  file(REMOVE_RECURSE "${_staging}" "${_third}/libwebrtc")
  file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${_staging}")
  file(RENAME "${_staging}/linux-x64-release" "${_third}/libwebrtc")
  file(REMOVE_RECURSE "${_staging}")
  file(WRITE "${_stamp}" "${LIBWEBRTC_SHA256}\n")
endif()
message(STATUS "libwebrtc: ${LIBWEBRTC_RELEASE}, SHA-256 ${LIBWEBRTC_SHA256}")
