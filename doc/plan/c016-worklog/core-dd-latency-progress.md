START:2026-09-04 detached=b72622bb2d scope=core/src,core/tests existing_untracked=core/build-main-readonly
CHECK:HEAD=b72622bb2d release_lib=core/build/lib/libzlink.so.0.17.0 existing_user_change_preserved
BASELINE_PAIR1:0.17 1024=886284,1.329,4.281,10.825 4096=385890,625.503,1251.922,1333.721;0.15.1 1024=806588,0.871,1.861,2.978 4096=373248,595.539,877.421,976.035 (throughput,mean,p95,p99)
BASELINE_PAIR2:0.17 1024=862712,1.471,6.400,12.764 4096=374848,632.296,1291.437,1385.812;0.15.1 1024=806857,0.853,1.930,4.221 4096=353280,627.752,917.484,1042.331 (throughput,mean,p95,p99)
INSTRUMENT:normal5s current send=10271670 backpressure=1079 writable=1079 completion_recv=2158 poll_wait=11/poll_events=1079;0.15.1 send=8975748 backpressure=546 completion_recv=0 poll_wait(new_api)=0;callgrind1s current register=25 activate=25 completion_recv=50 vs old activate=23
D713:1024=423170,0.109,0.151,0.210 4096=215880,0.115,0.153,0.209;contract-B boundary introduces high-throughput queued regime;experiment remove redundant synchronous process_submit_commands from wait registration
TEST_EXPERIMENT:phase3_completion+wake_invariants+stream_blocking_wakeup passed 5/5 on dev build
AFTER_EXPERIMENT:pair1 1024=891062,1.220,2.456,4.081 4096=359129,643.458,1298.002,1382.032;pair2 1024=854048,1.174,2.357,5.109 4096=333944,713.529,1314.853,1386.328
GATE_DEV:138/139 pass;test_backpressure_oneway_matrix_pubsub_regression double-free once then standalone PASS;unrelated intermittent retained as blocker
GATE_RELEASE:release-gate build PASS;hotpath cells all PASS max_ratio=1.0109;git_diff_check PASS
SUMMARY:/home/hep7hep7/project/zlink-work/c016/core-dd-latency-summary.md
EXIT:0
