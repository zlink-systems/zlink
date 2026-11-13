#!/usr/bin/env bash
set -u
ulimit -c 0
case_root=/root/jobs/fx/cx/v21/s033/cmfix

run_suite() {
    local suite_name="$1"
    local suite_build="$2"
    shift 2
    local task_mem_kib
    task_mem_kib=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
    if [ "$task_mem_kib" -lt 12582912 ]; then
        printf 'MemAvailable below 12 GiB before %s: %s KiB\n' "$suite_name" "$task_mem_kib"
        exit 75
    fi
    printf 'START %s MemAvailable=%s KiB\n' "$suite_name" "$task_mem_kib"
    ctest --test-dir "$suite_build" "$@" --output-on-failure > "$case_root/raw/$suite_name.log" 2>&1
    local suite_result=$?
    cp "$suite_build/Testing/Temporary/LastTest.log" "$case_root/raw/$suite_name-cases.log"
    tail -18 "$case_root/raw/$suite_name.log"
    if [ "$suite_result" -ne 0 ]; then
        printf 'FAILED %s status=%s\n' "$suite_name" "$suite_result"
        exit "$suite_result"
    fi
}

sha256sum "$case_root/core-build/lib/libzlink.so.1.23.0" > "$case_root/raw/final-tested-core-sha256.txt"
ldd "$case_root/cpp-build/test_cpp_contract_multipart_resume" > "$case_root/raw/final-cpp-runtime.log"
run_suite core-unit-contract "$case_root/core-build" -L 'unittest|contract' -j2
run_suite core-additional-contract "$case_root/core-build" -R contract -LE 'unittest|contract' -j1
run_suite cpp-unit-contract "$case_root/cpp-build" -L 'unit|contract' -j1
