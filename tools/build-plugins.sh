#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds every example plugin (plugins/examples/<name>/*.c -> out/plugins/<name>.dll) and the Lua
# host (plugins/lua -> out/plugins/bb_lua.dll, with third_party/lua-5.4.7), with the MSYS2 CLANG64
# compiler, against the port's plugin header (port/include/bbhost_plugin.h), and the tool's own
# tests (tests/*.c -> out/tests/). Warnings are errors in our code (not in Lua's own sources): an
# example that stops building means the API changed under it.
#
#   bash tools/build-plugins.sh             build
#   bash tools/build-plugins.sh --install   build, then copy the DLLs to port/plugins/, where a
#                                           source-tree game run loads them (docs/PLUGINS.md)
#   bash tools/build-plugins.sh hello       only the examples named
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/out/plugins"
install=0
names=()
for arg in "$@"; do
    if [[ $arg == --install ]]; then install=1; else names+=("$arg"); fi
done
all=0
if (( ! ${#names[@]} )); then
    all=1
    for dir in "$root"/plugins/examples/*/; do names+=("$(basename "$dir")"); done
fi
mkdir -p "$out"
clang=/c/msys64/clang64/bin/clang.exe
[[ -x $clang ]] || { echo "No $clang: install MSYS2 CLANG64 (packaging/windows/README.md in port/)"; exit 2; }

failed=0
for name in "${names[@]}"; do
    src="$root/plugins/examples/$name"
    sources=("$src"/*.c)
    if [[ ! -f ${sources[0]} ]]; then echo "FAIL $name: no .c files in $src"; failed=$((failed + 1)); continue; fi
    if PATH="/c/msys64/clang64/bin:$PATH" "$clang" -shared -O2 -std=c11 -Wall -Wextra -Werror \
            -I"$root/port/include" "${sources[@]}" -o "$out/$name.dll" 2> "$out/$name.log"; then
        echo "built $name -> out/plugins/$name.dll"
    else
        echo "FAIL $name (log: out/plugins/$name.log)"; sed 's/^/    /' "$out/$name.log" | head -20
        failed=$((failed + 1))
    fi
done
# The Lua host: Lua's sources once into objects (their warnings are theirs), then bb_lua.c with -Werror.
build_lua() {
    local objs="$root/out/lua-obj" lua="$root/third_party/lua-5.4.7/src"
    mkdir -p "$objs"
    for c in "$lua"/*.c; do
        local o="$objs/$(basename "${c%.c}").o"
        if [[ ! -f $o || $c -nt $o ]]; then
            PATH="/c/msys64/clang64/bin:$PATH" "$clang" -O2 -std=c11 -w -c "$c" -o "$o" || return 1
        fi
    done
    PATH="/c/msys64/clang64/bin:$PATH" "$clang" -shared -O2 -std=c11 -Wall -Wextra -Werror -I"$root/port/include" \
        -I"$lua" "$root/plugins/lua/bb_lua.c" "$objs"/*.o -o "$out/bb_lua.dll"
}
if (( all )); then
    if build_lua 2> "$out/bb_lua.log"; then
        echo "built lua -> out/plugins/bb_lua.dll"
    else
        echo "FAIL lua (log: out/plugins/bb_lua.log)"; sed 's/^/    /' "$out/bb_lua.log" | head -20
        failed=$((failed + 1))
    fi
    # The tool's own tests.
    mkdir -p "$root/out/tests"
    for t in "$root"/tests/*.c; do
        name=$(basename "${t%.c}" | tr '_' '-')
        if PATH="/c/msys64/clang64/bin:$PATH" "$clang" -O2 -std=c11 -Wall -Wextra -Werror -I"$root/port/include" "$t" \
                -o "$root/out/tests/$name.exe" 2> "$root/out/tests/$name.log"; then
            echo "built test $name"
        else
            echo "FAIL test $name (log: out/tests/$name.log)"; failed=$((failed + 1))
        fi
    done
fi
if (( install && ! failed )); then
    mkdir -p "$root/port/plugins"
    for name in "${names[@]}"; do cp "$out/$name.dll" "$root/port/plugins/"; done
    (( all )) && cp "$out/bb_lua.dll" "$root/port/plugins/"
    echo "installed into port/plugins/ (a game run from port/ loads them; delete them to stop)"
fi
exit $(( failed ? 1 : 0 ))
