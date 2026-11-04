#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds and runs the port's tests on Windows: every CMake "*-test" target (C/C++), then the
# Python suite. The port's build.sh stops after bb-probe.exe on Windows (no --test there), so
# this is the one command for both the coder and the reviewer. Results are compared against
# the baseline in docs/WORKPLAN.md (Phase 0): exit 0 only when nothing outside that baseline
# fails.
#
# Run from any shell:  bash tools/test-port.sh            (C and Python)
#                      bash tools/test-port.sh --c-only   (or --py-only)
# Needs: a configured port build (bash build.sh in MSYS2 CLANG64, once), python.org Python
# (BB_PYTHON, default the per-user 3.13), and the game dump (BB_TEST_GAME_DIR) for the tests
# that read the player's own files.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
port="$root/port"
python=${BB_PYTHON:-/c/Users/gard1/AppData/Local/Programs/Python/Python313/python.exe}
export BB_TEST_GAME_DIR=${BB_TEST_GAME_DIR:-C:/games/Bloodborne/game}
logs=${BB_TEST_LOGS:-$root/out/test-logs}
mkdir -p "$logs"
mode=${1:-all}

# Known failures on Windows at the baseline (upstream 2.3.2, recorded 2026-11-04). Anything
# else failing is new.
known_c='camera-motion-test scene-resolution-test taa-shader-test upscaler-support-test trophy-data-test trophy-history-test trophy-toast-test'
known_py='test_https_trusts_the_shipped_roots_and_explains_certificate_errors test_launcher_ingame_sync test_mod_symlinks_rejected test_host_nvidia_and_compiler_are_exposed_without_host_libc test_live_resolution_without_sed_or_grep test_settings_navigation test_setup_wizard test_host_driver_can_be_supplied_outside_hidden_nix_store test_live_resolution_setting_and_gpu_check test_outputs_other_than_1080p_patch_the_render_size_and_restarts_recompute_it'

new_failures=0
is_known() { [[ " $2 " == *" $1 "* ]]; }

run_c() {
    # In the CLANG64 environment, so the test executables find their DLLs.
    local script="$logs/c-tests.sh"
    cat > "$script" <<EOF
cd '$port' && source ./msys2-env.sh
for t in \$(ninja -C out/gpu -t targets all 2>/dev/null | grep -o -E '^[a-z0-9-]+-test(\.exe)?:' | sed -E 's/(\.exe)?://' | sort -u); do
    if ! ninja -C out/gpu "\$t" > '$logs'/build-\$t.log 2>&1; then echo "BUILDFAIL \$t"; continue; fi
    if timeout 300 out/gpu/\$t.exe > '$logs'/run-\$t.log 2>&1; then echo "PASS \$t"; else echo "FAIL \$t"; fi
done
EOF
    local pass=0
    while read -r status name; do
        case $status in
        PASS) pass=$((pass + 1)) ;;
        *) if is_known "$name" "$known_c"; then echo "  known  $status $name"
           else echo "  NEW    $status $name (log: $logs/*-$name.log)"; new_failures=$((new_failures + 1)); fi ;;
        esac
    done < <(MSYSTEM=CLANG64 CHERE_INVOKING=1 /c/msys64/usr/bin/bash.exe -l "$script" 2>&1)
    echo "C/C++: $pass passed"
}

run_py() {
    local log="$logs/python.log"
    (cd "$port" && "$python" -m unittest discover -s tests -p 'test_*.py' > "$log" 2>&1)
    grep -a -E '^Ran |^(OK|FAILED)' "$log" | sed 's/^/Python: /'
    while read -r name; do
        if is_known "$name" "$known_py"; then echo "  known  $name"
        else echo "  NEW    $name (log: $log)"; new_failures=$((new_failures + 1)); fi
    done < <(grep -a -E '^(ERROR|FAIL): ' "$log" | awk '{print $2}')
}

[[ $mode != --py-only ]] && run_c
[[ $mode != --c-only ]] && run_py
if (( new_failures )); then echo "$new_failures failure(s) outside the baseline"; exit 1; fi
echo 'No failures outside the baseline'
