2026-09-05T17:18:14+09:00
2026-09-05T17:20:41.441750 correlation work budget refusal ENOBUFS becomes EAGAIN; generic token recheck sees physical pipe ready; configure passed, dev -j3 build running; token contract gap under review
2026-09-05T17:22:49+09:00
2026-09-05T17:24:59.405241 dev build passed; public API repro compiled; corrected test completion wait to zlink_poller API (legacy zlink_poll rejects POLLCOMPLETION); runtime remains unchanged pending correlation token contract
2026-09-05T17:25:43.393518+09:00 repro-2 started: ctest --test-dir core/build-writable -R ^test_request_writable_credit$ -V
2026-09-05T17:25:43.559252+09:00 repro-2 completed exit=8 elapsed_s=0.17
2026-09-05T17:25:43.559481+09:00 repro-3 started: ctest --test-dir core/build-writable -R ^test_request_writable_credit$ -V
2026-09-05T17:25:43.725701+09:00 repro-3 completed exit=8 elapsed_s=0.17
2026-09-05T17:25:43.725947+09:00 repro-4 started: ctest --test-dir core/build-writable -R ^test_request_writable_credit$ -V
2026-09-05T17:25:43.892069+09:00 repro-4 completed exit=8 elapsed_s=0.17
2026-09-05T17:25:43.892208+09:00 repro-5 started: ctest --test-dir core/build-writable -R ^test_request_writable_credit$ -V
2026-09-05T17:25:44.057338+09:00 repro-5 completed exit=8 elapsed_s=0.16
2026-09-05T17:25:44.057557+09:00 integration started: ctest --test-dir core/build-writable -L integration -j2 --output-on-failure
2026-09-05T17:26:44.058856+09:00 integration running elapsed_s=60
2026-09-05T17:27:44.059315+09:00 integration running elapsed_s=120
2026-09-05T17:28:44.060108+09:00 integration running elapsed_s=180
2026-09-05T17:29:18.715248+09:00 integration completed exit=8 elapsed_s=214.66
2026-09-05T17:29:18.715351+09:00 full started: ctest --test-dir core/build-writable -j2 -E hotpath_gate --output-on-failure
2026-09-05T17:30:18.716559+09:00 full running elapsed_s=60
2026-09-05T17:31:18.717346+09:00 full running elapsed_s=120
2026-09-05T17:32:18.717877+09:00 full running elapsed_s=180
2026-09-05T17:33:18.718361+09:00 full running elapsed_s=240
2026-09-05T17:33:27.914136+09:00 full completed exit=8 elapsed_s=249.2
2026-09-05T17:34:44.309904+09:00 summary finalized; correlation token spec gap blocks runtime change; new repro 5/5 detects defect; integration 125/126 and full 175/176 passed with only new repro failing; diff checks passed; no runtime/hotpath edits
EXIT:2
