#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Waits until the reviewer's report for a commit exists (code_review/reports/<repo>-<sha>.md),
# then prints only its verdict line and its finding headings, not the whole report. Run it in
# the background and get on with reading or planning; it returns when the review is in.
#
#   bash tools/wait-review.sh tool          the tool repo's HEAD
#   bash tools/wait-review.sh port          the port's HEAD
#   bash tools/wait-review.sh tool 37b73c6  a given commit
#
# Gives up after BB_REVIEW_WAIT seconds (default 3 hours), exit 1.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
repo=${1:?usage: wait-review.sh tool|port [sha]}
case $repo in
    tool) dir=$root ;;
    port) dir=$root/port ;;
    *) echo "repo is tool or port"; exit 2 ;;
esac
sha=${2:-$(git -C "$dir" rev-parse --short=7 HEAD)}
sha=${sha:0:7}
report="$root/code_review/reports/$repo-$sha.md"
limit=${BB_REVIEW_WAIT:-10800}
waited=0
until [[ -f $report ]]; do
    (( waited >= limit )) && { echo "no review of $repo-$sha after $waited s"; exit 1; }
    sleep 30; waited=$((waited + 30))
done
sleep 5  # let the reviewer finish writing it
echo "review: $report"
grep -a -m1 '^**Verdict' "$report"
grep -a -E '^### ' "$report" || echo "(no findings)"
