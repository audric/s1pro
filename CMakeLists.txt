cmake_minimum_required(VERSION 3.19)
project(s1pro VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Bluetooth backend: "bluez" talks to the Linux Bluetooth service directly and is the one the
# tool was developed on; "qt" uses Qt Bluetooth and works on macOS, Windows and Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(default_bluetooth bluez)
else()
  set(default_bluetooth qt)
endif()
set(S1PRO_BLUETOOTH ${default_bluetooth} CACHE STRING "Bluetooth backend: bluez or qt")

set(sources src/main.cpp src/radio.cpp src/radio.h src/tables.cpp src/tables.h src/link.h)
if(S1PRO_BLUETOOTH STREQUAL "bluez")
  find_package(Qt6 REQUIRED COMPONENTS Widgets DBus)
  list(APPEND sources src/link_bluez.cpp)
  set(bluetooth_library Qt6::DBus)
elseif(S1PRO_BLUETOOTH STREQUAL "qt")
  find_package(Qt6 REQUIRED COMPONENTS Widgets Bluetooth)
  list(APPEND sources src/link_qt.cpp)
  set(bluetooth_library Qt6::Bluetooth)
else()
  message(FATAL_ERROR "S1PRO_BLUETOOTH must be bluez or qt")
endif()

add_executable(s1pro WIN32 MACOSX_BUNDLE ${sources})
target_link_libraries(s1pro PRIVATE Qt6::Widgets ${bluetooth_library})
if(MSVC)
  target_compile_options(s1pro PRIVATE /W3 /utf-8)
else()
  target_compile_options(s1pro PRIVATE -Wall -Wextra)
endif()
set_target_properties(s1pro PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/Info.plist.in
  MACOSX_BUNDLE_BUNDLE_NAME "s1pro"
  MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.s1pro"
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
  MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
