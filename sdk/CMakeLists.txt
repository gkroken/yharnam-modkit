# SPDX-License-Identifier: GPL-3.0-or-later
# Builds every example plugin in examples/c/ (one folder each, all its .c files), into
# <build>/<name>.dll.
#
#   cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=cmake/bb-clang-mingw.cmake
#   cmake --build build
cmake_minimum_required(VERSION 3.20)
project(bloodborne_plugin_examples C)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/BloodbornePlugin.cmake")

file(GLOB _bb_examples LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/examples/c/*")
foreach(_dir IN LISTS _bb_examples)
    if(IS_DIRECTORY "${_dir}")
        get_filename_component(_name "${_dir}" NAME)
        file(GLOB _sources "${_dir}/*.c")
        if(_sources)
            bb_add_plugin(${_name} ${_sources})
            target_compile_options(${_name} PRIVATE -Werror)
        endif()
    endif()
endforeach()
