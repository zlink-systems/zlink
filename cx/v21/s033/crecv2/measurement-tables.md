## Final before/after measurements

Median of three runs; throughput in messages/s, CPU in µs/completed message. csubmit uses condition B and paired binding medians. Ladder denominators are its unchanged before references (rung 0: 223,019.24; unlimited binding: 246,721.55 messages/s), so ladder F/B is not a newly paired binding measurement. Full per-run values, ranges, and actual thread IDs are in `measurements.json` and `threads.csv`.

| Cell | Before msg/s | After msg/s | Change | Before F/B | After F/B | Caller CPU before → after | Source CPU before → after | Target CPU before → after |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| cladder rung 9 | 45,050.7 | 46,180.3 | +2.5% | 0.202 | 0.207 | 20.148 → 19.484 | 91.766 → 88.027 | 72.469 → 71.267 |
| cladder unlimited Framework | 49,558.2 | 50,625.2 | +2.2% | 0.201 | 0.205 | 18.906 → 18.029 | 86.043 → 83.073 | 65.632 → 63.973 |
| csubmit request, binding | 246,826.1 | 250,627.3 | +1.5% | — | — | 4.069 → 4.057 | 9.693 → 9.735 | 10.027 → 9.802 |
| csubmit request, Framework | 49,602.0 | 51,940.2 | +4.7% | 0.201 | 0.207 | 18.326 → 17.585 | 84.121 → 81.215 | 62.637 → 59.341 |
| csubmit send, binding | 616,974.0 | 612,012.0 | -0.8% | — | — | 1.448 → 1.416 | 3.061 → 3.024 | 3.437 → 3.360 |
| csubmit send, Framework | 173,204.0 | 175,907.9 | +1.6% | 0.281 | 0.287 | 4.884 → 4.817 | 10.249 → 10.067 | 15.352 → 14.625 |

Raw paths (each directory contains `results.json`, source/target logs, and `capture/` snapshots):

| Cell | Before | After |
|---|---|---|
| cladder rung 9 | `/root/jobs/fx/cx/v21/s033/cladder/raw/rung9/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/cladder/raw/rung9/run2/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/cladder/raw/rung9/run3/zlink-framework-cpp-request-backpressure-1024` | `/root/jobs/fx/cx/v21/s033/crecv2/ladder/raw/rung9/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/ladder/raw/rung9/run2/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/ladder/raw/rung9/run3/zlink-framework-cpp-request-backpressure-1024` |
| cladder unlimited Framework | `/root/jobs/fx/cx/v21/s033/cladder/raw/rung9-unbounded/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/cladder/raw/rung9-unbounded/run2/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/cladder/raw/rung9-unbounded/run3/zlink-framework-cpp-request-backpressure-1024` | `/root/jobs/fx/cx/v21/s033/crecv2/ladder/raw/rung9-unbounded/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/ladder/raw/rung9-unbounded/run2/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/ladder/raw/rung9-unbounded/run3/zlink-framework-cpp-request-backpressure-1024` |
| csubmit request, binding | `/root/jobs/fx/cx/v21/s033/csubmit/measure/1/request-backpressure/branch/run1/zlink-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/2/request-backpressure/branch/run1/zlink-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/3/request-backpressure/branch/run1/zlink-cpp-request-backpressure-1024` | `/root/jobs/fx/cx/v21/s033/crecv2/measure/run1/zlink-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run2/zlink-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run3/zlink-cpp-request-backpressure-1024` |
| csubmit request, Framework | `/root/jobs/fx/cx/v21/s033/csubmit/measure/1/request-backpressure/branch/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/2/request-backpressure/branch/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/3/request-backpressure/branch/run1/zlink-framework-cpp-request-backpressure-1024` | `/root/jobs/fx/cx/v21/s033/crecv2/measure/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run2/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run3/zlink-framework-cpp-request-backpressure-1024` |
| csubmit send, binding | `/root/jobs/fx/cx/v21/s033/csubmit/measure/1/send-saturation/branch/run1/zlink-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/2/send-saturation/branch/run1/zlink-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/3/send-saturation/branch/run1/zlink-cpp-send-saturation-1024` | `/root/jobs/fx/cx/v21/s033/crecv2/measure/run1/zlink-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run2/zlink-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run3/zlink-cpp-send-saturation-1024` |
| csubmit send, Framework | `/root/jobs/fx/cx/v21/s033/csubmit/measure/1/send-saturation/branch/run1/zlink-framework-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/2/send-saturation/branch/run1/zlink-framework-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/3/send-saturation/branch/run1/zlink-framework-cpp-send-saturation-1024` | `/root/jobs/fx/cx/v21/s033/crecv2/measure/run1/zlink-framework-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run2/zlink-framework-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run3/zlink-framework-cpp-send-saturation-1024` |

### Per-thread CPU

Names below are the recorded thread role or Linux comm plus descending CPU rank within that role/comm in each run, then the three-run median. They are not stable thread IDs across processes. Every thread with at least 0.5 µs/message in either cohort is listed; all smaller threads and each actual TID remain in `threads.csv` and `measurements.json`. Unnamed `bench_cpp_*` workers are intentionally not assigned an inferred execution role.

| Cell | Endpoint | Recorded thread/rank | Before µs/msg | After µs/msg |
|---|---|---|---:|---:|
| cladder rung 9 | source | `caller #1` | 20.148 | 19.484 |
| cladder rung 9 | source | `bench_cpp_clien #1` | 18.393 | 17.772 |
| cladder rung 9 | source | `ZLINKbg/IO/0 #1` | 18.233 | 17.783 |
| cladder rung 9 | source | `ZLINKbg/IO/2 #1` | 16.936 | 16.360 |
| cladder rung 9 | source | `bench_cpp_clien #2` | 2.203 | 2.113 |
| cladder rung 9 | source | `bench_cpp_clien #3` | 2.201 | 2.108 |
| cladder rung 9 | source | `bench_cpp_clien #4` | 2.200 | 2.103 |
| cladder rung 9 | source | `bench_cpp_clien #5` | 2.200 | 2.103 |
| cladder rung 9 | source | `bench_cpp_clien #6` | 2.198 | 2.101 |
| cladder rung 9 | source | `bench_cpp_clien #7` | 2.197 | 2.098 |
| cladder rung 9 | source | `bench_cpp_clien #8` | 2.190 | 2.094 |
| cladder rung 9 | source | `bench_cpp_clien #9` | 2.189 | 2.089 |
| cladder rung 9 | target | `ZLINKbg/IO/1 #1` | 17.874 | 17.241 |
| cladder rung 9 | target | `receive #1` | 15.891 | 15.693 |
| cladder rung 9 | target | `ZLINKbg/IO/0 #1` | 13.670 | 13.934 |
| cladder rung 9 | target | `app-worker #1` | 8.130 | 10.273 |
| cladder rung 9 | target | `app-worker #2` | 5.469 | 7.536 |
| cladder rung 9 | target | `app-worker #3` | 5.152 | 2.075 |
| cladder rung 9 | target | `app-worker #4` | 1.381 | 0.000 |
| cladder rung 9 | target | `bench_cpp_frame #1` | 0.548 | 0.552 |
| cladder rung 9 | target | `bench_cpp_frame #2` | 0.526 | 0.533 |
| cladder rung 9 | target | `bench_cpp_frame #3` | 0.516 | 0.507 |
| cladder rung 9 | target | `bench_cpp_frame #4` | 0.508 | 0.505 |
| cladder unlimited Framework | source | `caller #1` | 18.906 | 18.029 |
| cladder unlimited Framework | source | `ZLINKbg/IO/0 #1` | 16.471 | 15.687 |
| cladder unlimited Framework | source | `bench_cpp_clien #1` | 16.012 | 16.240 |
| cladder unlimited Framework | source | `ZLINKbg/IO/2 #1` | 15.248 | 15.338 |
| cladder unlimited Framework | source | `bench_cpp_clien #2` | 2.380 | 2.183 |
| cladder unlimited Framework | source | `bench_cpp_clien #3` | 2.367 | 2.166 |
| cladder unlimited Framework | source | `bench_cpp_clien #4` | 2.367 | 2.164 |
| cladder unlimited Framework | source | `bench_cpp_clien #5` | 2.366 | 2.163 |
| cladder unlimited Framework | source | `bench_cpp_clien #6` | 2.360 | 2.160 |
| cladder unlimited Framework | source | `bench_cpp_clien #7` | 2.358 | 2.160 |
| cladder unlimited Framework | source | `bench_cpp_clien #8` | 2.355 | 2.152 |
| cladder unlimited Framework | source | `bench_cpp_clien #9` | 2.352 | 2.145 |
| cladder unlimited Framework | target | `ZLINKbg/IO/1 #1` | 16.029 | 16.143 |
| cladder unlimited Framework | target | `receive #1` | 14.019 | 13.127 |
| cladder unlimited Framework | target | `ZLINKbg/IO/0 #1` | 12.619 | 12.088 |
| cladder unlimited Framework | target | `app-worker #1` | 6.520 | 6.430 |
| cladder unlimited Framework | target | `app-worker #2` | 4.864 | 4.259 |
| cladder unlimited Framework | target | `app-worker #3` | 2.933 | 2.812 |
| cladder unlimited Framework | target | `app-worker #4` | 2.131 | 1.580 |
| cladder unlimited Framework | target | `app-worker #5` | 0.857 | 1.414 |
| cladder unlimited Framework | target | `app-worker #6` | 0.787 | 0.961 |
| csubmit request, binding | source | `caller #1` | 4.069 | 4.057 |
| csubmit request, binding | source | `ZLINKbg/IO/2 #1` | 2.948 | 2.877 |
| csubmit request, binding | source | `ZLINKbg/IO/0 #1` | 2.641 | 2.674 |
| csubmit request, binding | target | `ZLINKbg/IO/1 #1` | 3.517 | 3.428 |
| csubmit request, binding | target | `receive #1` | 3.408 | 3.300 |
| csubmit request, binding | target | `ZLINKbg/IO/0 #1` | 2.233 | 2.252 |
| csubmit request, Framework | source | `caller #1` | 18.326 | 17.585 |
| csubmit request, Framework | source | `ZLINKbg/IO/0 #1` | 15.957 | 15.363 |
| csubmit request, Framework | source | `bench_cpp_clien #1` | 15.723 | 15.536 |
| csubmit request, Framework | source | `ZLINKbg/IO/2 #1` | 14.884 | 14.834 |
| csubmit request, Framework | source | `bench_cpp_clien #2` | 2.377 | 2.187 |
| csubmit request, Framework | source | `bench_cpp_clien #3` | 2.374 | 2.181 |
| csubmit request, Framework | source | `bench_cpp_clien #4` | 2.366 | 2.179 |
| csubmit request, Framework | source | `bench_cpp_clien #5` | 2.366 | 2.172 |
| csubmit request, Framework | source | `bench_cpp_clien #6` | 2.366 | 2.168 |
| csubmit request, Framework | source | `bench_cpp_clien #7` | 2.366 | 2.164 |
| csubmit request, Framework | source | `bench_cpp_clien #8` | 2.356 | 2.158 |
| csubmit request, Framework | source | `bench_cpp_clien #9` | 2.351 | 2.153 |
| csubmit request, Framework | target | `ZLINKbg/IO/1 #1` | 16.237 | 15.245 |
| csubmit request, Framework | target | `receive #1` | 13.603 | 12.892 |
| csubmit request, Framework | target | `ZLINKbg/IO/0 #1` | 12.658 | 11.993 |
| csubmit request, Framework | target | `app-worker #1` | 9.660 | 9.151 |
| csubmit request, Framework | target | `app-worker #2` | 9.073 | 7.281 |
| csubmit request, Framework | target | `app-worker #3` | 0.000 | 1.944 |
| csubmit send, binding | source | `ZLINKbg/IO/0 #1` | 1.583 | 1.595 |
| csubmit send, binding | source | `caller #1` | 1.448 | 1.416 |
| csubmit send, binding | target | `ZLINKbg/IO/0 #1` | 1.473 | 1.438 |
| csubmit send, binding | target | `receive #1` | 1.341 | 1.331 |
| csubmit send, Framework | source | `caller #1` | 4.884 | 4.817 |
| csubmit send, Framework | source | `ZLINKbg/IO/0 #1` | 4.846 | 4.784 |
| csubmit send, Framework | target | `ZLINKbg/IO/0 #1` | 4.349 | 3.961 |
| csubmit send, Framework | target | `receive #1` | 4.227 | 4.195 |
| csubmit send, Framework | target | `app-worker #1` | 2.923 | 2.803 |
| csubmit send, Framework | target | `app-worker #2` | 2.777 | 2.732 |
