# SPDX-License-Identifier: GPL-3.0-or-later
# A plugin of your own: copy this folder anywhere, rename my_plugin, then
#
#   cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=<sdk>/cmake/bb-clang-mingw.cmake -DBB_SDK_DIR=<sdk>
#   cmake --build build
#
# and copy build/my_plugin.dll into the game's plugins folder. <sdk> is the folder holding this
# SDK's README.md; left out, it's taken to be this folder's parent (the template inside the SDK).
cmake_minimum_required(VERSION 3.20)
project(my_plugin C)

set(BB_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "The Bloodborne plugin SDK")
if(NOT EXISTS "${BB_SDK_DIR}/cmake/BloodbornePlugin.cmake")
    message(FATAL_ERROR "BB_SDK_DIR (${BB_SDK_DIR}) is not the plugin SDK: pass -DBB_SDK_DIR=<sdk>")
endif()
include("${BB_SDK_DIR}/cmake/BloodbornePlugin.cmake")

bb_add_plugin(my_plugin my_plugin.c)
