#!/bin/bash
# run.sh <name> [seconds] : run the headless harness in runs/<name>, print the log tail
N=$1; T=${2:-30}; D=/home/claude/breakdown-recomp/runs/$N
rm -rf "$D"; mkdir -p "$D"; cd "$D"
timeout "$T" /home/claude/breakdown-recomp/build/harness/breakdown /home/claude/game > run.out 2>&1; echo "exit $?"
grep -v ExQueryNonVolatileSetting logs/*.log | tail -${TAIL:-30}
