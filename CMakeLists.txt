# It worth requiring an older version here, if anyone has problems with
# 2.8.  But that means finding somebody to test against those versions...
cmake_minimum_required(VERSION 3.5)

# Declare our project name and version.
project(pstsdk)
set(pstsdk_VERSION_MAJOR 0)
set(pstsdk_VERSION_MINOR 4)
set(pstsdk_VERSION_PATCH 0)

# Turn on testing.
enable_testing()

# Make our pstsdk/ subdirectory available for headers.
include_directories("${PROJECT_SOURCE_DIR}")

# Export compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set up our compiler flags.
if(MSVC)
  set(CMAKE_CXX_FLAGS "/DPSTSDK_VALIDATION_LEVEL_FULL /EHsc /nologo /std:c++17 /W4 /WX /wd4244 /wd2220")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # https://clang.llvm.org/docs/DiagnosticsReference.html#wincompatible-ms-struct 
  set(CMAKE_CXX_FLAGS "-Wall -Wno-error=incompatible-ms-struct -g -DPSTSDK_VALIDATION_LEVEL_FULL -std=c++17")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  set(CMAKE_CXX_FLAGS "-Wall -g -DPSTSDK_VALIDATION_LEVEL_FULL -std=c++17")
endif()

# Make sure we have Boost.
include(FindBoost)
find_package(Boost 1.42.0 REQUIRED)
if(Boost_FOUND)
  include_directories(${Boost_INCLUDE_DIRS})
endif()

# Use iconv if we have it.  This is required on non-Win32 platforms,
# because we don't necessarily know the encoding of wchar_t.
find_library(ICONV_LIBRARY NAMES iconv)

# Compile our unit tests.
add_subdirectory(test)

# Install our headers.  There may be a more elegant way to do this.
file(GLOB pstsdk_util_headers pstsdk/util/*.h)
install(FILES ${pstsdk_util_headers} DESTINATION include/pstsdk/util)
file(GLOB pstsdk_disk_headers pstsdk/disk/*.h)
install(FILES ${pstsdk_disk_headers} DESTINATION include/pstsdk/disk)
file(GLOB pstsdk_ndb_headers pstsdk/ndb/*.h)
install(FILES ${pstsdk_ndb_headers} DESTINATION include/pstsdk/ndb)
file(GLOB pstsdk_ltp_headers pstsdk/ltp/*.h)
install(FILES ${pstsdk_ltp_headers} DESTINATION include/pstsdk/ltp)
file(GLOB pstsdk_pst_headers pstsdk/pst/*.h)
install(FILES ${pstsdk_pst_headers} DESTINATION include/pstsdk/pst)
file(GLOB pstsdk_toplevel_headers pstsdk/*.h)
install(FILES ${pstsdk_toplevel_headers} DESTINATION include/pstsdk)

# Custom target to generate compile_commands.json for headers using compdb
add_custom_target(compdb
    COMMAND compdb -p ${CMAKE_BINARY_DIR} list > ${PROJECT_SOURCE_DIR}/compile_commands.json
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating compile_commands.json with compdb"
    VERBATIM
)
