# SPDX-License-Identifier: GPL-3.0-or-later
# CMake toolchain file for Bloodborne PC (yharnam-pc) plugins: clang targeting 64-bit Windows
# with the MinGW ABI (x86_64-w64-windows-gnu).
#
# Why not MSVC: a function the game calls directly (register_hle) must use the System V calling
# convention, BB_GUEST_ABI in bbhost_plugin.h. clang and GCC can express it; MSVC can't.
#
#   cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=<sdk>/cmake/bb-clang-mingw.cmake
#
# Tested with MSYS2's CLANG64 environment (its clang.exe, cmake.exe and ninja.exe, from any
# shell). Another clang that targets x86_64-w64-windows-gnu with a MinGW sysroot (llvm-mingw)
# should work too: point BB_CLANG at it. Not tested: cross builds from Linux.

if(NOT CMAKE_HOST_WIN32)
    set(CMAKE_SYSTEM_NAME Windows)
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
endif()

# -DBB_CLANG=<path> picks one; otherwise clang on PATH, then MSYS2's CLANG64.
find_program(BB_CLANG NAMES clang clang.exe PATHS "C:/msys64/clang64/bin" "$ENV{MSYS2_ROOT}/clang64/bin"
             DOC "The clang to build with")
if(NOT BB_CLANG)
    message(FATAL_ERROR "No clang found. Install MSYS2 and its CLANG64 toolchain "
                        "(pacman -S mingw-w64-clang-x86_64-toolchain), or set BB_CLANG.")
endif()

set(CMAKE_C_COMPILER "${BB_CLANG}")
set(CMAKE_C_COMPILER_TARGET x86_64-w64-windows-gnu)
get_filename_component(_bb_bin "${BB_CLANG}" DIRECTORY)
find_program(_bb_clangxx NAMES clang++ clang++.exe HINTS "${_bb_bin}" NO_DEFAULT_PATH)
if(_bb_clangxx)
    set(CMAKE_CXX_COMPILER "${_bb_clangxx}")
    set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-windows-gnu)
endif()
find_program(CMAKE_RC_COMPILER NAMES llvm-windres windres HINTS "${_bb_bin}")
