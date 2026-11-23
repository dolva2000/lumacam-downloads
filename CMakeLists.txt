cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
option(ENABLE_BONJOUR "Discover phones with Bonjour / mDNS (dns_sd)" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/lumacam-source.cpp
    src/lumacam-source.hpp
    src/lcp/protocol.hpp
    src/net/socket.cpp
    src/net/socket.hpp
    src/net/usbmux.cpp
    src/net/usbmux.hpp
    src/net/discovery.cpp
    src/net/discovery.hpp
    src/net/lan.cpp
    src/net/lan.hpp
    src/net/connector.cpp
    src/net/connector.hpp
    src/decoder/video-decoder.cpp
    src/decoder/video-decoder.hpp
    src/decoder/vt-decoder.cpp
    src/decoder/ffmpeg-decoder.cpp
)

# Video decoding: VideoToolbox on macOS, FFmpeg (with hardware acceleration) elsewhere.
if(APPLE)
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE "$<LINK_LIBRARY:FRAMEWORK,VideoToolbox.framework>"
            "$<LINK_LIBRARY:FRAMEWORK,CoreMedia.framework>"
            "$<LINK_LIBRARY:FRAMEWORK,CoreVideo.framework>"
            "$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>"
  )
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavutil)
  endif()
  if(FFMPEG_FOUND)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::FFMPEG)
  else()
    # Windows: FFmpeg comes from the pre-built obs-deps on CMAKE_PREFIX_PATH.
    find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h REQUIRED)
    find_library(AVCODEC_LIBRARY avcodec REQUIRED)
    find_library(AVUTIL_LIBRARY avutil REQUIRED)
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${AVCODEC_INCLUDE_DIR}")
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "${AVCODEC_LIBRARY}" "${AVUTIL_LIBRARY}")
  endif()
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE LUMACAM_HAVE_FFMPEG)
endif()

if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 iphlpapi)
endif()

# Bonjour discovery: built into macOS; avahi-compat-libdns_sd on Linux; Bonjour SDK on Windows.
if(ENABLE_BONJOUR)
  if(APPLE)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE LUMACAM_HAVE_DNSSD)
  else()
    find_path(DNSSD_INCLUDE_DIR dns_sd.h PATH_SUFFIXES avahi-compat-libdns_sd)
    find_library(DNSSD_LIBRARY NAMES dns_sd dnssd)
    if(DNSSD_INCLUDE_DIR AND DNSSD_LIBRARY)
      target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${DNSSD_INCLUDE_DIR}")
      target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "${DNSSD_LIBRARY}")
      target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE LUMACAM_HAVE_DNSSD)
    else()
      message(WARNING "dns_sd not found: Bonjour discovery disabled (USB and manual IP still work)")
    endif()
  endif()
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
