#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds every example plugin (plugins/examples/<name>/*.c -> out/plugins/<name>.dll) with the
# MSYS2 CLANG64 compiler, against the port's plugin header (port/include/bbhost_plugin.h).
# Warnings are errors: an example that stops building means the API changed under it.
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
if (( ! ${#names[@]} )); then
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
if (( install && ! failed )); then
    mkdir -p "$root/port/plugins"
    for name in "${names[@]}"; do cp "$out/$name.dll" "$root/port/plugins/"; done
    echo "installed into port/plugins/ (a game run from port/ loads them; delete them to stop)"
fi
exit $(( failed ? 1 : 0 ))
