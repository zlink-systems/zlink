#!/bin/bash
set -euo pipefail
cd /root/worktree/zlink-3867a
job=/root/jobs/fx/cx/v21/s033/sfix
build=/root/jobs/fx/cx/v21/s033/salloc/build
file=framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp
check_memory() {
 while (( $(awk '/MemAvailable/{print $2}' /proc/meminfo) < 14680064 )); do sleep 5; done
 awk '/MemAvailable/{print strftime("%FT%T"),$2}' /proc/meminfo >> "$job/compile-memory.tsv"
}
cp "$file" "$job/raw-route-final.cpp"
trap 'cp "$job/raw-route-final.cpp" "$file"' EXIT
python3 - "$file" <<'PY'
import sys,pathlib
p=pathlib.Path(sys.argv[1]);s=p.read_text();old='admission = _request_receive_source (target_routing_id);'
assert s.count(old)==1
p.write_text(s.replace(old,'admission = _request_receive_source (raw_bytes_t (target_routing_id.begin (), target_routing_id.end ()));'))
PY
check_memory
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$build" --target test_cpp_framework_raw_route_port_contract \
 --parallel 3 -- -k 0 > "$job/borrow-negative-build.log" 2>&1
set +e
/root/jobs/fx/cx/measure-shared.sh ctest --test-dir "$build" -R '^test_cpp_framework_raw_route_port_contract$' \
 --parallel 3 --output-on-failure > "$job/borrow-negative.log" 2>&1
rc=$?
set -e
cp "$job/raw-route-final.cpp" "$file"
trap - EXIT
[[ $rc -ne 0 ]]
grep -F 'observed.data () == target.data ()' "$job/borrow-negative.log"
check_memory
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$build" --parallel 3 -- -k 0 > "$job/borrow-restored-build.log" 2>&1
/root/jobs/fx/cx/measure-shared.sh ctest --test-dir "$build" -L 'framework-unit|framework-contract' \
 --parallel 3 --output-on-failure --output-junit "$job/unit-contract.xml" > "$job/unit-contract.log" 2>&1
