cmake_minimum_required(VERSION 3.10)
project(carplay-cpp LANGUAGES CXX VERSION 0.1.0)

# C++17 keeps older ARM BSP toolchains viable (e.g. Linux 4.9 / gcc 7+).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CP_BUILD_CLI "Build headless CLI (doctor / status)" ON)
option(CP_BUILD_TESTS "Build unit tests" ON)
option(CP_UI_QT5 "Build optional Qt5 settings UI" ON)
option(CP_PLATFORM_POSIX "POSIX sockets / files platform" ON)
option(CP_PLATFORM_LIBUSB "Desktop USB probing via libusb" OFF)
option(CP_WITH_OPENSSL "Link OpenSSL for MFi / pairing crypto" ON)

add_library(cp_protocol
  src/protocol/version.cpp
  src/protocol/buffer_util.cpp
  src/protocol/csm_framer.cpp
  src/protocol/tlv8.cpp
  src/protocol/iap_link.cpp
  src/protocol/iap_accessory.cpp
  src/protocol/mfi_signer.cpp
  src/protocol/pair_setup.cpp
  src/protocol/pair_verify.cpp
  src/protocol/bplist.cpp
)
target_include_directories(cp_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_options(cp_protocol PRIVATE -Wall -Wextra -Wpedantic)

add_library(cp_media
  src/media/frame_clock.cpp
)
target_include_directories(cp_media PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(cp_media PUBLIC cp_protocol)

add_library(cp_platform
  src/platform/null_bluetooth.cpp
  src/platform/null_wifi.cpp
  src/platform/posix_storage.cpp
  src/platform/linux_system_info.cpp
)
target_include_directories(cp_platform PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(cp_platform PUBLIC cp_protocol)

if(CP_PLATFORM_POSIX)
  target_compile_definitions(cp_platform PUBLIC CP_HAVE_POSIX=1)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(cp_platform PRIVATE src/platform/bluez_bluetooth.cpp)
  target_compile_definitions(cp_platform PUBLIC CP_HAVE_BLUEZ=1)
  target_link_libraries(cp_platform PUBLIC bluetooth)
  message(STATUS "BlueZ RFCOMM: enabled")
endif()

find_package(Threads REQUIRED)
target_link_libraries(cp_platform PUBLIC Threads::Threads)

if(CP_WITH_OPENSSL)
  find_package(OpenSSL)
  if(OpenSSL_FOUND)
    target_compile_definitions(cp_protocol PUBLIC CP_HAVE_OPENSSL=1)
    target_link_libraries(cp_protocol PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "OpenSSL: ${OPENSSL_VERSION}")
  else()
    message(STATUS "OpenSSL: not found (crypto stubs only)")
  endif()
endif()

if(CP_PLATFORM_LIBUSB)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
  endif()
  if(TARGET PkgConfig::LIBUSB)
    target_sources(cp_platform PRIVATE src/platform/libusb_probe.cpp)
    target_link_libraries(cp_platform PUBLIC PkgConfig::LIBUSB)
    target_compile_definitions(cp_platform PUBLIC CP_HAVE_LIBUSB=1)
    message(STATUS "libusb-1.0: enabled")
  else()
    message(STATUS "libusb-1.0: requested but not found")
  endif()
endif()

add_library(cp_session
  src/session/runtime_config.cpp
  src/session/controller.cpp
  src/session/receiver.cpp
  src/session/control_session.cpp
  src/session/session_services.cpp
  src/session/audio_port.cpp
  src/session/settings_io.cpp
)
target_include_directories(cp_session PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(cp_session PUBLIC cp_protocol cp_media cp_platform)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(FFMPEG_CORE QUIET libavcodec libavutil libswscale libswresample)
endif()
if(FFMPEG_CORE_FOUND)
  target_compile_definitions(cp_session PUBLIC CP_HAVE_FFMPEG=1)
  target_include_directories(cp_session PRIVATE ${FFMPEG_CORE_INCLUDE_DIRS})
  target_link_libraries(cp_session PUBLIC ${FFMPEG_CORE_LIBRARIES})
  target_compile_options(cp_session PRIVATE ${FFMPEG_CORE_CFLAGS_OTHER})
endif()

if(CP_BUILD_CLI)
  add_executable(carplay-cli apps/cli/main.cpp)
  target_link_libraries(carplay-cli PRIVATE cp_session)
endif()

if(CP_BUILD_TESTS)
  enable_testing()
  add_executable(csm_framer_test tests/csm_framer_test.cpp)
  target_link_libraries(csm_framer_test PRIVATE cp_protocol)
  add_test(NAME csm_framer_test COMMAND csm_framer_test)

  add_executable(tlv8_test tests/tlv8_test.cpp)
  target_link_libraries(tlv8_test PRIVATE cp_protocol)
  add_test(NAME tlv8_test COMMAND tlv8_test)

  add_executable(iap_link_test tests/iap_link_test.cpp)
  target_link_libraries(iap_link_test PRIVATE cp_protocol)
  add_test(NAME iap_link_test COMMAND iap_link_test)
endif()

if(CP_UI_QT5)
  find_package(Qt5 COMPONENTS Widgets QUIET)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET libavcodec libavutil libswscale)
  endif()
  if(Qt5Widgets_FOUND AND FFMPEG_FOUND)
    add_executable(carplay-ui
      apps/ui-qt/main.cpp
      apps/ui-qt/mainwindow.cpp
      apps/ui-qt/video_surface.cpp
      apps/ui-qt/h264_decoder.cpp
      apps/ui-qt/video_surface.hpp
      apps/ui-qt/mainwindow.hpp
    )
    target_include_directories(carplay-ui PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(carplay-ui PRIVATE cp_session Qt5::Widgets ${FFMPEG_LIBRARIES})
    target_compile_options(carplay-ui PRIVATE ${FFMPEG_CFLAGS_OTHER})
    set_target_properties(carplay-ui PROPERTIES AUTOMOC ON)
  else()
    message(STATUS "Qt5/FFmpeg not found; UI skipped (need qtbase5-dev libavcodec-dev libswscale-dev)")
  endif()
endif()

install(TARGETS cp_protocol cp_media cp_platform cp_session
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)
install(DIRECTORY include/carplay DESTINATION include)

message(STATUS "carplay-cpp ${PROJECT_VERSION}")
message(STATUS "  CLI=${CP_BUILD_CLI} tests=${CP_BUILD_TESTS} qt5=${CP_UI_QT5}")
