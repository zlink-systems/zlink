2026-09-05 05:40 KST START detached b774608b60; scope bindings/cpp/**; existing only core/build and core/build-dev symlinks; guide §2.1/§2.4 and §4 reviewed.
2026-09-05 05:41 KST C++ Release perf binaries built under bindings/cpp/build (JOBS=4); 10-client 1s PUBSUB 64/4096 profile baseline complete, report perf_cpp_multi_linux_20260905_053935_cpp_pubsub_profile_baseline.txt.
2026-09-05 05:45 KST C/C++ callgrind publisher+subscriber profiles complete for 64B/4096B; 3s 10-client cross matrix isolates subscriber penalty (~5.3/5.6%), publisher ~neutral; candidate is cached empty-output receive branch.
2026-09-05 05:48 KST §2.1/§2.4 verdict NO-GO: no per-message binding allocation/lock/std::function or payload copy; cached-empty branch still needs one size query to preserve empty/non-empty rollback contract, wrapper/topic output costs are public contract; no code change adopted.
2026-09-05 05:49 KST requested 100-client 5s C++ after complete: 738.9/883.7/935.6/785.6/55.6 Kmsg/s; paired-C ratios 117.9/149.6/131.5/119.9/84.5%, aggregate 120.7%, latency 0.97x; report perf_cpp_multi_linux_20260905_054800.txt.
2026-09-05 05:51 KST Gate PASS: full contract 16/16, samples 7/7, socket contract direct repeat 5/5, git diff --check; tracked/public-header diff 0.
2026-09-05 05:52 KST Summary written to cpp-perf-pubsub-pass1-summary.md; no code change adopted; blockers limited to attribution variance and 65536B individual 84.5%.
EXIT:0
