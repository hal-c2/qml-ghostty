cmake_minimum_required(VERSION 3.21)

project(qml-ghostty VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Quick)
qt_standard_project_setup(REQUIRES 6.8)
qt_policy(SET QTP0001 NEW)

# libghostty-vt: headers plus the static archive from
# scripts/build-libghostty-vt.sh. An embedding project (hal-c2) can point
# GHOSTTY_VT_INCLUDE_DIR at its own vendored headers as long as the pinned
# revisions match.
set(GHOSTTY_VT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libghostty-vt" CACHE PATH
  "Prefix holding include/ghostty and lib/libghostty-vt.a")
set(GHOSTTY_VT_INCLUDE_DIR "${GHOSTTY_VT_ROOT}/include" CACHE PATH "libghostty-vt headers")
set(GHOSTTY_VT_LIBRARY "${GHOSTTY_VT_ROOT}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}ghostty-vt${CMAKE_STATIC_LIBRARY_SUFFIX}"
  CACHE FILEPATH "libghostty-vt library")
if(NOT EXISTS "${GHOSTTY_VT_INCLUDE_DIR}/ghostty/vt.h" OR NOT EXISTS "${GHOSTTY_VT_LIBRARY}")
  message(FATAL_ERROR "libghostty-vt not found under ${GHOSTTY_VT_ROOT}; run scripts/build-libghostty-vt.sh")
endif()

if(NOT TARGET ghostty::vt)
  add_library(ghostty::vt UNKNOWN IMPORTED)
  set_target_properties(ghostty::vt PROPERTIES
    IMPORTED_LOCATION "${GHOSTTY_VT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${GHOSTTY_VT_INCLUDE_DIR}")
endif()

add_subdirectory(src)
