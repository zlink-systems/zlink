#!/usr/bin/env bash
set -u
ulimit -c 0
case_root=/root/jobs/fx/cx/v21/s033/cmfix
task_mem_kib=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
if [ "$task_mem_kib" -lt 12582912 ]; then exit 75; fi
env ZLINK_TEST_CASE=test_dealer_multipart_reply_uses_complete_record_empty_exception \
  ctest --test-dir "$case_root/core-build" -R '^test_phase3_request_reply_contract$' \
  --output-on-failure > "$case_root/raw/reply-context-fixed.log" 2>&1
suite_result=$?
cp "$case_root/core-build/Testing/Temporary/LastTest.log" "$case_root/raw/reply-context-fixed-cases.log"
tail -15 "$case_root/raw/reply-context-fixed.log"
if [ "$suite_result" -ne 0 ]; then exit "$suite_result"; fi
exec bash "$case_root/run_final_tests.sh"
