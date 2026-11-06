#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Makes the plugin SDK release package (sdk/README.md says what's in it) from this tree:
#   out/sdk/bloodborne-plugin-sdk-api<N>/      the package
#   out/sdk/bloodborne-plugin-sdk-api<N>.zip   the same, zipped
# <N> is BB_PLUGIN_API_VERSION in port/include/bbhost_plugin.h. bin/bb_lua.dll is built here from
# the package's own src/bb_lua (Release, with its CMakeLists), so the source it ships is the
# binary's. Needs MSYS2 CLANG64 (clang, cmake, ninja).
#
#   bash tools/make-sdk.sh           make it
#   bash tools/make-sdk.sh --check   make it, then prove it works on its own: configure and build
#                                    the package's examples, its template (in place and copied
#                                    elsewhere) and bb_lua from its src/ again, with CMake and
#                                    Ninja; the rebuilt bb_lua.dll must equal bin/'s byte for byte,
#                                    and lua-host-test (tools/build-plugins.sh builds it) must
#                                    pass on bin/'s; each DLL must export bb_plugin_info and import
#                                    only Windows' own DLLs; only source, doc and build files may
#                                    be in it besides bin/bb_lua.dll (no game files). The test
#                                    gate runs this.
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
work="$out/check"
export PATH="$bin:$PATH"
win() { cygpath -m "$1"; }
toolchain="$(win "$pkg/cmake/bb-clang-mingw.cmake")"

failed=0
fail() { echo "FAIL $*"; failed=$((failed + 1)); }
build() {  # build <build dir> <source dir> [cmake args...]: configure and build, log in <build dir>.log
    local dir=$1 src=$2; shift 2
    rm -rf "$dir"
    if ! { cmake -G Ninja -S "$(win "$src")" -B "$(win "$dir")" -DCMAKE_TOOLCHAIN_FILE="$toolchain" "$@" &&
           cmake --build "$(win "$dir")"; } > "$dir.log" 2>&1; then
        fail "$(basename "$dir"): does not build (log: ${dir#"$root/"}.log)"
        grep -m5 -E 'error|Error' "$dir.log" | sed 's/^/    /'
        return 1
    fi
}

# The package.
rm -rf "$pkg" "$out/$name.zip" "$work"
mkdir -p "$pkg"/{include,bin,docs,examples/c,examples/lua,src/bb_lua,tools} "$work"
cp -r "$root/sdk/." "$pkg/"
cp "$root/LICENSE" "$pkg/"
cp "$root/tools/game-world.ps1" "$pkg/tools/"  # the in-world runner: -Install <port tree>
cp "$header" "$pkg/include/"
cp "$root/port/docs/PLUGINS.md" "$root/docs/LUA.md" "$pkg/docs/"
for dir in "$root"/plugins/examples/*/; do
    mkdir -p "$pkg/examples/c/$(basename "$dir")"
    cp "$dir"*.c "$pkg/examples/c/$(basename "$dir")/"
done
cp "$root"/plugins/lua/examples/*.lua "$pkg/examples/lua/"
cp "$root/plugins/lua/bb_lua.c" "$pkg/src/bb_lua/"
cp -r "$root/third_party/lua-5.4.7" "$pkg/src/bb_lua/"
build "$out/build-bb_lua" "$pkg/src/bb_lua" || exit 1
cp "$out/build-bb_lua/bb_lua.dll" "$pkg/bin/"
rev() {  # rev <git dir> [pathspec...]: the commit, with "+changes" when the tree differs from it
    local dir=$1 sha; shift
    sha=$(git -C "$dir" rev-parse --short=7 HEAD)
    git -C "$dir" diff --quiet --ignore-submodules=dirty HEAD -- . "$@" 2>/dev/null || sha="$sha+changes"
    echo "$sha"
}
# The port's own submodules' dirt (fsr-vulkan) isn't ours; a moved port commit in the tool is.
printf 'plugin API %s\ntool %s\nport %s\n' "$api" "$(rev "$root")" "$(rev "$root/port" ':!gpu/third_party')" > "$pkg/VERSION"

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
dll_ok() {  # dll_ok <dll>: exports bb_plugin_info; imports only Windows' own DLLs
    local dll=$1 dump bad
    [[ -f $dll ]] || { fail "$(basename "$dll") was not built"; return; }
    dump=$("$bin/llvm-objdump.exe" -p "$dll")
    grep -qE '[[:space:]]bb_plugin_info$' <<< "$dump" || fail "$(basename "$dll") does not export bb_plugin_info"
    bad=$(sed -n -E 's/^[[:space:]]*DLL Name: (.*)$/\1/p' <<< "$dump" | tr -d '\r' |
          grep -viE '^(kernel32|user32|advapi32|ucrtbase|api-ms-win-[a-z0-9-]+)\.dll$' | sort -u | tr '\n' ' ')
    [[ -z $bad ]] || fail "$(basename "$dll") needs DLLs a player won't have: $bad"
}

if build "$work/examples" "$pkg"; then
    for dir in "$pkg"/examples/c/*/; do dll_ok "$work/examples/$(basename "$dir").dll"; done
fi
build "$work/template" "$pkg/template" && dll_ok "$work/template/my_plugin.dll"
# The README's strict C11 switch must build the template too.
if build "$work/template-c11" "$pkg/template" -DCMAKE_C_EXTENSIONS=OFF; then
    grep -q -- '-std=c11' "$work/template-c11/build.ninja" || fail "template-c11: -DCMAKE_C_EXTENSIONS=OFF did not give -std=c11"
    dll_ok "$work/template-c11/my_plugin.dll"
fi
cp -r "$pkg/template" "$work/mine"
build "$work/mine-build" "$work/mine" -DBB_SDK_DIR="$(win "$pkg")" && dll_ok "$work/mine-build/my_plugin.dll"

# bin/bb_lua.dll: the DLL that ships. Its source is src/bb_lua: built again from there (in another
# folder; the build is reproducible), it must come out the same, byte for byte.
shipped="$pkg/bin/bb_lua.dll"
dll_ok "$shipped"
if build "$work/bb_lua" "$pkg/src/bb_lua" && ! cmp -s "$work/bb_lua/bb_lua.dll" "$shipped"; then
    fail "bin/bb_lua.dll is not what src/bb_lua builds (rebuilt: out/sdk/check/bb_lua/bb_lua.dll)"
fi
host_test="$root/out/tests/lua-host-test.exe"
if [[ ! -x $host_test ]]; then
    fail "no $host_test to test bin/bb_lua.dll with (run tools/build-plugins.sh)"
elif ! timeout 120 "$host_test" "$shipped" "$pkg/examples/lua" > "$work/bb_lua-test.log" 2>&1; then
    fail "lua-host-test fails on bin/bb_lua.dll (log: out/sdk/check/bb_lua-test.log)"
fi

# A copied template whose SDK is not found must say so, not build against something else.
cp -r "$pkg/template" "$work/lost"
if cmake -G Ninja -S "$(win "$work/lost")" -B "$(win "$work/lost-build")" -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        > "$work/lost.log" 2>&1 || ! grep -q 'pass -DBB_SDK_DIR=<sdk>' "$work/lost.log"; then
    fail "a template without BB_SDK_DIR did not stop and ask for -DBB_SDK_DIR (log: out/sdk/check/lost.log)"
fi

# Only these kinds of file: sources, docs, build files and the one DLL. A game file has no place.
bad=$(cd "$pkg" && find . -type f | grep -vE '\.(c|h|lua|md|cmake)$|^\./tools/game-world\.ps1$|/(CMakeLists\.txt|LICENSE|VERSION)$|^\./bin/bb_lua\.dll$')
[[ -z $bad ]] || fail "unexpected files in the package: $(echo $bad)"
listed=$("$python" -I -c 'import sys, zipfile; print(len(zipfile.ZipFile(sys.argv[1]).namelist()))' "$(win "$out/$name.zip")")
files=$(find "$pkg" -type f | wc -l)
[[ $listed == "$files" ]] || fail "the zip holds $listed files, the package $files"

if (( failed )); then echo "SDK check: $failed problem(s)"; exit 1; fi
echo "SDK check: out/sdk/$name.zip ($files files, $(head -3 "$pkg/VERSION" | tr '\n' ' ')) builds $(ls "$pkg"/examples/c | wc -l) examples, the template and bb_lua the same again"
