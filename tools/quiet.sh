#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs a command with its output in a log file, and prints one line when it succeeds. When it
# fails, prints only the lines that look like errors (at most 40) and the last 15 lines, plus the
# log's path. Meant for agents and CI, where a build's full output costs more than it tells.
#
#   bash tools/quiet.sh <name> <command> [args...]
#   bash tools/quiet.sh lua-build bash tools/build-plugins.sh
#
# The log is out/logs/<name>.log (BB_QUIET_LOGS to change). Exit status is the command's.
set -uo pipefail
[[ $# -ge 2 ]] || { echo "usage: quiet.sh <name> <command> [args...]"; exit 2; }
root=$(cd "$(dirname "$0")/.." && pwd)
logs=${BB_QUIET_LOGS:-$root/out/logs}
mkdir -p "$logs"
name=$1; shift
log="$logs/$name.log"
start=$SECONDS
"$@" > "$log" 2>&1
status=$?
if (( status == 0 )); then
    echo "ok $name ($((SECONDS - start)) s, log: $log)"
    exit 0
fi
echo "FAILED $name: exit $status ($((SECONDS - start)) s, log: $log)"
errors=$(grep -a -n -i -E 'error|fail|undefined reference|assert|check failed|traceback|exception|PROBLEM' "$log" \
         | grep -a -v -i -E '(^|[: ])warning:' | head -40)  # warnings are not errors; -Werror ones are
[[ -n $errors ]] && { echo "--- error lines"; echo "$errors"; }
echo "--- last 15 lines"
tail -n 15 "$log"
exit $status
