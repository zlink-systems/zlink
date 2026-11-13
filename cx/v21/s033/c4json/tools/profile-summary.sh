#!/usr/bin/env bash
set -euo pipefail
for d in /root/jobs/fx/cx/v21/s033/c4json/raw/"$1"-L*; do
 perf report --stdio -i "$d/perf.data" --no-children -g none --sort comm,dso,symbol --percent-limit .5 > "$d/perf-self.txt" 2>&1
 perf report --stdio -i "$d/perf.data" --children --sort comm,symbol --percent-limit 1 > "$d/perf-callgraph.txt" 2>&1
done
