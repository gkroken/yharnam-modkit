#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the port (port/build.sh) in MSYS2 CLANG64 from any shell, quietly: one line when it
# builds, the error lines and the log's path when it doesn't (tools/quiet.sh). The game must not
# be running (bb-probe.exe is locked while it runs).
#
#   bash tools/build-port.sh
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
if tasklist //FI "IMAGENAME eq bb-probe.exe" 2>/dev/null | grep -qi bb-probe; then
    echo "bb-probe.exe is running: close the game first"; exit 2
fi
exec bash "$root/tools/quiet.sh" port-build \
    env MSYSTEM=CLANG64 CHERE_INVOKING=1 /c/msys64/usr/bin/bash.exe -lc "cd '$root/port' && bash build.sh"
