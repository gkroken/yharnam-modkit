#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the example plugins and the Lua host and runs the tool's own tests, then builds and runs
# the port's tests on Windows: every CMake "*-test" target (C/C++), then the Python suite. The port's build.sh stops after bb-probe.exe on Windows (no --test there), so
# this is the one command for both the coder and the reviewer.
#
# It compares against tools/test-baseline.txt and exits 0 only when:
# - nothing fails that the baseline doesn't list, matched by full test id;
# - at least as many tests ran and passed as the baseline's floors, and no more were skipped
#   (a broken build or a missing Python must not look like a clean run).
# A known failure that now passes is printed, so the list can shrink.
#
# Run from any shell:  bash tools/test-port.sh            (C and Python)
#                      bash tools/test-port.sh --c-only   (or --py-only)
#                      bash tools/test-port.sh --quiet    (only problems and the last line:
#                                                          for agents; combines with the others)
# Needs: a configured port build (bash build.sh in MSYS2 CLANG64, once), python.org Python
# (BB_PYTHON, default the per-user 3.13) with capstone, keystone-engine and numpy
# (pip install --user capstone keystone-engine numpy), and the game dump
# (BB_TEST_GAME_DIR) for the tests that read the player's own files.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
port="$root/port"
baseline="$root/tools/test-baseline.txt"
python=${BB_PYTHON:-/c/Users/gard1/AppData/Local/Programs/Python/Python313/python.exe}
export BB_TEST_GAME_DIR=${BB_TEST_GAME_DIR:-C:/games/Bloodborne/game}
# test_cpu_compat's allocator tests read the same dump under their own name.
export BB_CPU_GAME_DIR=${BB_CPU_GAME_DIR:-$BB_TEST_GAME_DIR}
logs=${BB_TEST_LOGS:-$root/out/test-logs}
mkdir -p "$logs"
mode=all
quiet=0
for arg in "$@"; do
    case $arg in
        --quiet) quiet=1 ;;
        --c-only|--py-only) mode=$arg ;;
        *) echo "unknown option $arg"; exit 2 ;;
    esac
done
say() { (( quiet )) || echo "$@"; }  # progress lines; problems always print

problems=0
problem() { echo "  PROBLEM $*"; problems=$((problems + 1)); }
floor() { awk -v k="$1" '$1 == k { print $2 }' "$baseline"; }
known() { awk -v k="$1" '$1 == k { print $2 }' "$baseline"; }

[[ -f $baseline ]] || { echo "No $baseline"; exit 2; }

# Compares what failed with what the baseline lists: new failures are problems, known ones that
# passed are reported.
compare() {  # compare <kind: c-known|py-known> <file of failed ids>
    local id
    while read -r id; do
        [[ -z $id ]] && continue
        if known "$1" | grep -qxF -- "$id"; then say "  known   $id"; else problem "new failure: $id"; fi
    done < "$2"
    while read -r id; do
        grep -qxF -- "$id" "$2" || echo "  NOTE    known failure passed now (remove it from the baseline): $id"
    done < <(known "$1")
}

run_c() {
    # In the CLANG64 environment, so the test executables find their DLLs.
    local script="$logs/c-tests.sh" results="$logs/c-results.txt"
    cat > "$script" <<EOF
cd '$port' && source ./msys2-env.sh
if ! targets=\$(ninja -C out/gpu -t targets all 2>&1); then echo "NOTARGETS \$targets"; exit 0; fi
for t in \$(printf '%s\n' "\$targets" | grep -o -E '^[a-z0-9-]+-test(\.exe)?:' | sed -E 's/(\.exe)?://' | sort -u); do
    if ! ninja -C out/gpu "\$t" > '$logs'/build-\$t.log 2>&1; then echo "BUILDFAIL \$t"; continue; fi
    if timeout 300 out/gpu/\$t.exe > '$logs'/run-\$t.log 2>&1; then echo "PASS \$t"; else echo "FAIL \$t"; fi
done
EOF
    MSYSTEM=CLANG64 CHERE_INVOKING=1 /c/msys64/usr/bin/bash.exe -l "$script" > "$results" 2>&1
    # file-offsets-test (upstream) leaves port/bbport-offsets-<time>/ behind, empty folders by
    # then: remove only the empty ones, so a run doesn't grow the tree.
    find "$port" -maxdepth 1 -name 'bbport-offsets-*' -type d -exec find {} -depth -type d -empty -delete \; 2>/dev/null
    if grep -q '^NOTARGETS' "$results"; then
        problem "no C/C++ test targets: is the port configured (bash build.sh)? $(head -c 300 "$results")"
        return
    fi
    local targets pass
    targets=$(grep -c -E '^(PASS|FAIL|BUILDFAIL) ' "$results")
    pass=$(grep -c '^PASS ' "$results")
    grep -E '^(FAIL|BUILDFAIL) ' "$results" | awk '{print $2}' > "$logs/c-failed.txt"
    say "C/C++: $targets targets, $pass passed (logs: $logs/build-*.log, run-*.log)"
    compare c-known "$logs/c-failed.txt"
    (( targets >= $(floor c-min-targets) )) || problem "only $targets C/C++ targets; the baseline has $(floor c-min-targets)"
    (( pass >= $(floor c-min-pass) )) || problem "only $pass C/C++ targets passed; the baseline has $(floor c-min-pass)"
}

run_py() {
    local log="$logs/python.log"
    if ! "$python" -c 'import capstone, keystone, numpy' > "$logs/python-check.log" 2>&1; then
        problem "$python cannot run the suite with capstone, keystone and numpy (pip install --user capstone keystone-engine numpy): $(tail -1 "$logs/python-check.log")"
        return
    fi
    # The probe's DLLs (test_probe runs bb-probe.exe).
    (cd "$port" && PATH="/c/msys64/clang64/bin:$PATH" "$python" -m unittest discover -s tests -p 'test_*.py' > "$log" 2>&1)
    local ran skipped
    ran=$(grep -a -o -E '^Ran [0-9]+ tests?' "$log" | grep -o -E '[0-9]+')
    if [[ -z $ran ]]; then problem "the Python suite did not report a run: $(tail -3 "$log")"; return; fi
    skipped=$(grep -a -o -E 'skipped=[0-9]+' "$log" | grep -o -E '[0-9]+' || true)
    skipped=${skipped:-0}
    (( quiet )) || grep -a -E '^Ran |^(OK|FAILED)' "$log" | sed 's/^/Python: /'
    # The full id is in the parentheses: module.Class.method, or unittest.loader._FailedTest.<module>.
    grep -a -E '^(ERROR|FAIL): ' "$log" | sed -E 's/^[A-Z]+: [^ ]+ \(([^)]*)\).*$/\1/' > "$logs/py-failed.txt"
    compare py-known "$logs/py-failed.txt"
    (( ran >= $(floor py-min-ran) )) || problem "only $ran Python tests ran; the baseline has $(floor py-min-ran)"
    (( skipped <= $(floor py-max-skipped) )) || problem "$skipped Python tests skipped; the baseline allows $(floor py-max-skipped)"
}

# The example plugins (plugins/examples) must keep building against the port's header.
run_examples() {
    if bash "$root/tools/build-plugins.sh" > "$logs/examples.log" 2>&1; then
        say "Examples: $(grep -c '^built ' "$logs/examples.log") built"
    else
        problem "an example plugin does not build: $(grep '^FAIL' "$logs/examples.log" | head -3)"
    fi
}

# The tool's own tests (tests/*.c, built by build-plugins.sh): the Lua host against a fake host.
run_tool_tests() {
    local exe="$root/out/tests/lua-host-test.exe"
    if [[ ! -x $exe ]]; then problem "no $exe (did build-plugins.sh run?)"; return; fi
    if timeout 120 "$exe" "$root/out/plugins/bb_lua.dll" "$root/plugins/lua/examples" > "$logs/lua-host-test.log" 2>&1; then
        say "Tool tests: $(tail -1 "$logs/lua-host-test.log")"
    else
        problem "lua-host-test failed: $(grep -m1 'check failed' "$logs/lua-host-test.log")"
    fi
    if bash "$root/tests/quiet_test.sh" > "$logs/quiet-test.log" 2>&1; then
        say "Tool tests: $(tail -1 "$logs/quiet-test.log")"
    else
        problem "quiet-test failed: $(grep -m1 'check failed' "$logs/quiet-test.log")"
    fi
}

[[ $mode != --py-only ]] && run_examples
[[ $mode != --py-only ]] && run_tool_tests
[[ $mode != --py-only ]] && run_c
[[ $mode != --c-only ]] && run_py
if (( problems )); then echo "$problems problem(s) against the baseline"; exit 1; fi
echo 'Matches the baseline: no new failures, nothing fewer run'
