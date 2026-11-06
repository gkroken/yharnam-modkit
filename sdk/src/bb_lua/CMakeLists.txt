# SPDX-License-Identifier: GPL-3.0-or-later
# Builds bb_lua.dll, the Lua host in bin/, from its source here: bb_lua.c and Lua 5.4.7.
#
#   cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/bb-clang-mingw.cmake
#   cmake --build build
cmake_minimum_required(VERSION 3.20)
project(bb_lua C)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/BloodbornePlugin.cmake")

set(_lua "${CMAKE_CURRENT_SOURCE_DIR}/lua-5.4.7/src")
file(GLOB _lua_sources "${_lua}/*.c")
# Lua's own warnings are Lua's: its sources build without them, bb_lua.c with them as errors.
add_library(bb_lua_core STATIC ${_lua_sources})
set_target_properties(bb_lua_core PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_options(bb_lua_core PRIVATE -w)

bb_add_plugin(bb_lua bb_lua.c)
target_include_directories(bb_lua PRIVATE "${_lua}")
target_compile_options(bb_lua PRIVATE -Werror)
target_link_libraries(bb_lua PRIVATE bb_lua_core)
