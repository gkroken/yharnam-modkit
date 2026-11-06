# SPDX-License-Identifier: GPL-3.0-or-later
# bb_add_plugin(<name> <source>...): a Bloodborne PC plugin, <name>.dll, built against this SDK's
# include/bbhost_plugin.h.
#
#   include(<sdk>/cmake/BloodbornePlugin.cmake)
#   bb_add_plugin(my_plugin my_plugin.c)
#
# The DLL is named <name>.dll (no "lib" prefix) and links its runtime statically, so it needs
# nothing beyond Windows' own DLLs: a player's machine has no MSYS2. Warnings are on; add
# -Werror yourself if you want them to fail the build. The plugin's name inside the game comes
# from its bb_plugin_info, not from the file name, but keeping the two the same keeps
# plugins.ini readable.

get_filename_component(BB_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(BB_SDK_INCLUDE_DIR "${BB_SDK_DIR}/include")

function(bb_add_plugin name)
    if(NOT ARGN)
        message(FATAL_ERROR "bb_add_plugin(${name}): no sources")
    endif()
    add_library(${name} SHARED ${ARGN})
    set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}" C_STANDARD 11
                          C_STANDARD_REQUIRED ON)
    target_include_directories(${name} PRIVATE "${BB_SDK_INCLUDE_DIR}")
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    target_link_options(${name} PRIVATE -static)
endfunction()
