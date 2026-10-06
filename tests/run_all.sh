#!/bin/sh
# Runs every script in scripts/ through diff_oracle.py, in parallel, and
# prints one summary line per script.
cd "$(dirname "$0")"
ROM="${1:?usage: run_all.sh ROM [FRAMES]}"
FRAMES="${2:-2600}"
mkdir -p vectors/runs
ls scripts/*.txt | xargs -P 10 -I{} sh -c 'n=$(basename {} .txt); TRACE_MASK=FC00-FE00 python3 diff_oracle.py "'"$ROM"'" {} '"$FRAMES"' --out vectors/runs/$n > vectors/runs/$n.log 2>&1; head -2 vectors/runs/$n.log | tr "\n" " "; echo'
