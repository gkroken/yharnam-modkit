#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests tools/quiet.sh: one line on success; on failure the command's exit status, the error
# lines (a -Werror error tagged [-W...] among them, tool-5867c1b-F1) and not the warnings.
# Run by tools/test-port.sh; prints "quiet-test: N checks passed" or the failed check.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export BB_QUIET_LOGS=$work/logs
checks=0
check() {  # check <description> <command...>
    if "${@:2}"; then checks=$((checks + 1)); else echo "quiet-test: check failed: $1"; exit 1; fi
}

out=$(bash "$root/tools/quiet.sh" good true); status=$?
check "success exits 0" test $status -eq 0
check "success is one line" test "$(printf '%s\n' "$out" | wc -l)" -eq 1
check "success says ok" grep -q '^ok good ' <<< "$out"

cat > "$work/fails.sh" <<'EOF'
echo "x.c:3:5: error: unused variable 'x' [-Werror,-Wunused-variable]"
echo "y.c:9:1: error: call to undeclared function 'f' [-Wimplicit-function-declaration]"
echo "z.c:1:1: warning: something harmless (expected an error) [-Wextra]"
for i in $(seq 1 20); do echo "noise $i"; done
exit 3
EOF
out=$(bash "$root/tools/quiet.sh" bad bash "$work/fails.sh"); status=$?
check "failure keeps the exit status" test $status -eq 3
check "failure says FAILED" grep -q '^FAILED bad: exit 3' <<< "$out"
errors=$(sed -n '/^--- error lines/,/^--- last/p' <<< "$out")
check "a -Werror error is an error line" grep -q 'unused variable' <<< "$errors"
check "a [-W...] default error is an error line" grep -q 'undeclared function' <<< "$errors"
check "a warning is not an error line" bash -c '! grep -q "something harmless" <<< "$1"' _ "$errors"
check "the log has everything" grep -q 'noise 20' "$work/logs/bad.log"
echo "quiet-test: $checks checks passed"
