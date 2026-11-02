#!/bin/bash
# iter.sh <run name> [seconds]: rebuild the harness, run it, and on a crash print a backtrace
cd /home/claude/breakdown-recomp
JOBS=4 bash sdk/harness/build.sh -r . -g generated/game -o build/harness -n breakdown -x texturedumper 2>&1 | grep -E "error" && exit 1
TAIL=${TAIL:-12} scripts/run.sh "$1" "${2:-30}" | grep -v "vertex shader B0" | cut -c1-220
if grep -q "Segmentation\|Abort" runs/$1/run.out 2>/dev/null || [ "${PIPESTATUS[0]}" != 0 ]; then :; fi
cd runs/$1 && timeout 180 gdb -batch -ex run -ex "bt 10" -ex "x/2i \$pc" --args /home/claude/breakdown-recomp/build/harness/breakdown /home/claude/game 2>&1 | grep -E "^#|=>|SIG|exited" | head -14
