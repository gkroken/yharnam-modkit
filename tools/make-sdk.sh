#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Makes the plugin SDK release package (sdk/README.md says what's in it) from this tree:
#   out/sdk/bloodborne-plugin-sdk-api<N>/      the package
#   out/sdk/bloodborne-plugin-sdk-api<N>.zip   the same, zipped
# <N> is BB_PLUGIN_API_VERSION in port/include/bbhost_plugin.h. bin/bb_lua.dll is taken from
# out/plugins/, so run tools/build-plugins.sh first.
#
#   bash tools/make-sdk.sh           make it
#   bash tools/make-sdk.sh --check   make it, then prove it works on its own: configure and build
#                                    the package's examples, its template (in place and copied
#                                    elsewhere) and bb_lua from its src/, with CMake and Ninja;
#                                    check each DLL exports bb_plugin_info and imports only
#                                    Windows' own DLLs; check the package holds only the file
#                                    types it should (no game files). The test gate runs this.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
check=0
for arg in "$@"; do
    case $arg in
        --check) check=1 ;;
        *) echo "unknown option $arg"; exit 2 ;;
    esac
done
bin=/c/msys64/clang64/bin
python=${BB_PYTHON:-/c/Users/gard1/AppData/Local/Programs/Python/Python313/python.exe}
header="$root/port/include/bbhost_plugin.h"
api=$(sed -n -E 's/^#define BB_PLUGIN_API_VERSION ([0-9]+).*/\1/p' "$header")
[[ -n $api ]] || { echo "FAIL: no BB_PLUGIN_API_VERSION in $header"; exit 1; }
name="bloodborne-plugin-sdk-api$api"
out="$root/out/sdk"
pkg="$out/$name"
lua_dll="$root/out/plugins/bb_lua.dll"
[[ -f $lua_dll ]] || { echo "FAIL: no $lua_dll: run tools/build-plugins.sh first"; exit 1; }

rm -rf "$pkg" "$out/$name.zip"
mkdir -p "$pkg"/{include,bin,docs,examples/c,examples/lua,src/bb_lua}
cp -r "$root/sdk/." "$pkg/"
cp "$root/LICENSE" "$pkg/"
cp "$header" "$pkg/include/"
cp "$lua_dll" "$pkg/bin/"
cp "$root/port/docs/PLUGINS.md" "$root/docs/LUA.md" "$pkg/docs/"
for dir in "$root"/plugins/examples/*/; do
    mkdir -p "$pkg/examples/c/$(basename "$dir")"
    cp "$dir"*.c "$pkg/examples/c/$(basename "$dir")/"
done
cp "$root"/plugins/lua/examples/*.lua "$pkg/examples/lua/"
cp "$root/plugins/lua/bb_lua.c" "$pkg/src/bb_lua/"
cp -r "$root/third_party/lua-5.4.7" "$pkg/src/bb_lua/"
rev() {  # rev <git dir>: the commit, with "+changes" when the tree differs from it
    local sha
    sha=$(git -C "$1" rev-parse --short=7 HEAD)
    git -C "$1" diff --quiet HEAD -- . ':!gpu/third_party' 2>/dev/null || sha="$sha+changes"
    echo "$sha"
}
printf 'plugin API %s\ntool %s\nport %s\n' "$api" "$(rev "$root")" "$(rev "$root/port")" > "$pkg/VERSION"

"$python" -I -c '
import os, sys, zipfile
pkg, dest = sys.argv[1], sys.argv[2]
top = os.path.basename(pkg)
with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
    for d, dirs, files in os.walk(pkg):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, os.path.join(top, os.path.relpath(p, pkg)).replace(os.sep, "/"))
' "$(cygpath -w "$pkg")" "$(cygpath -w "$out/$name.zip")" || { echo "FAIL: zip"; exit 1; }
(( check )) || { echo "made out/sdk/$name.zip"; exit 0; }

# --check
failed=0
fail() { echo "FAIL $*"; failed=$((failed + 1)); }
work="$out/check"
rm -rf "$work"; mkdir -p "$work"
export PATH="$bin:$PATH"
win() { cygpath -m "$1"; }
toolchain="$(win "$pkg/cmake/bb-clang-mingw.cmake")"
build() {  # build <tag> <source dir> [cmake args...]: configure and build, log in $work/<tag>.log
    local tag=$1 src=$2; shift 2
    if ! { cmake -G Ninja -S "$(win "$src")" -B "$(win "$work/$tag")" -DCMAKE_TOOLCHAIN_FILE="$toolchain" "$@" &&
           cmake --build "$(win "$work/$tag")"; } > "$work/$tag.log" 2>&1; then
        fail "$tag: does not build (log: out/sdk/check/$tag.log)"; grep -m5 -E 'error|Error' "$work/$tag.log" | sed 's/^/    /'
        return 1
    fi
}
dll_ok() {  # dll_ok <dll>: exports bb_plugin_info; imports only Windows' own DLLs
    local dll=$1 dump bad
    [[ -f $dll ]] || { fail "$(basename "$dll") was not built"; return; }
    dump=$("$bin/llvm-objdump.exe" -p "$dll")
    grep -qE '[[:space:]]bb_plugin_info$' <<< "$dump" || fail "$(basename "$dll") does not export bb_plugin_info"
    bad=$(sed -n -E 's/^[[:space:]]*DLL Name: (.*)$/\1/p' <<< "$dump" | tr -d '\r' |
          grep -viE '^(kernel32|user32|advapi32|ucrtbase|api-ms-win-[a-z0-9-]+)\.dll$' | sort -u | tr '\n' ' ')
    [[ -z $bad ]] || fail "$(basename "$dll") needs DLLs a player won't have: $bad"
}

if build examples "$pkg"; then
    for dir in "$pkg"/examples/c/*/; do dll_ok "$work/examples/$(basename "$dir").dll"; done
fi
build template "$pkg/template" && dll_ok "$work/template/my_plugin.dll"
cp -r "$pkg/template" "$work/mine"
build mine-build "$work/mine" -DBB_SDK_DIR="$(win "$pkg")" && dll_ok "$work/mine-build/my_plugin.dll"
if build bb_lua "$pkg/src/bb_lua"; then
    dll_ok "$work/bb_lua/bb_lua.dll"
    # The shipped source must be the shipped DLL's: the Lua host's tests pass on the DLL built from
    # it too, with the package's example scripts.
    host_test="$root/out/tests/lua-host-test.exe"
    if [[ ! -x $host_test ]]; then
        fail "no $host_test to test src/bb_lua's build with (run tools/build-plugins.sh)"
    elif ! timeout 120 "$host_test" "$work/bb_lua/bb_lua.dll" "$pkg/examples/lua" > "$work/bb_lua-test.log" 2>&1; then
        fail "lua-host-test fails on the bb_lua.dll built from src/bb_lua (log: out/sdk/check/bb_lua-test.log)"
    fi
fi
# A copied template whose SDK is not found must say so, not build against something else.
cp -r "$pkg/template" "$work/lost"
if cmake -G Ninja -S "$(win "$work/lost")" -B "$(win "$work/lost-build")" -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        > "$work/lost.log" 2>&1 || ! grep -q 'pass -DBB_SDK_DIR=<sdk>' "$work/lost.log"; then
    fail "a template without BB_SDK_DIR did not stop and ask for -DBB_SDK_DIR (log: out/sdk/check/lost.log)"
fi

# Only these kinds of file: sources, docs, build files and the one DLL. A game file has no place.
bad=$(cd "$pkg" && find . -type f | grep -vE '\.(c|h|lua|md|cmake)$|/(CMakeLists\.txt|LICENSE|VERSION)$|^\./bin/bb_lua\.dll$')
[[ -z $bad ]] || fail "unexpected files in the package: $(echo $bad)"
listed=$("$python" -I -c 'import sys, zipfile; print(len(zipfile.ZipFile(sys.argv[1]).namelist()))' "$(win "$out/$name.zip")")
files=$(find "$pkg" -type f | wc -l)
[[ $listed == "$files" ]] || fail "the zip holds $listed files, the package $files"

if (( failed )); then echo "SDK check: $failed problem(s)"; exit 1; fi
echo "SDK check: out/sdk/$name.zip ($files files) builds $(ls "$pkg"/examples/c | wc -l) examples, the template and bb_lua"
