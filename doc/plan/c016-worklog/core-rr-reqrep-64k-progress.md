START:2026-09-05 detached@87057e8654; scope=core/src/**,core/tests/**; untracked core/build-main-readonly preserved
BUILD_BASELINE:release LTO lib-only PASS (JOBS=6)
REPRO_017_PASS1:RR=27.332Kops/s,1.970ms; DR=29.007Kops/s,1.949ms; report=perf_c_multi_linux_20260905_020358.txt
REPRO_0151_PASS1:RR=29.929Kops/s,1.145ms; DR=29.489Kops/s,1.049ms; report=perf_c_multi_linux_20260905_020506.txt; ratios017/0151 RR=0.913 DR=0.984
REPRO_017_PASS2:RR=27.748Kops/s,1.869ms; DR=28.214Kops/s,1.878ms; report=perf_c_multi_linux_20260905_020609.txt
REPRO_0151_PASS2:RR=30.540Kops/s,1.025ms; DR=25.228Kops/s,1.152ms; report=perf_c_multi_linux_20260905_020707.txt; ratios017/0151 RR=0.909 DR=1.118; paired RR regression reproduced twice
INSTRUMENT:RR wait/writable=72469/72469 route=6.16ms ready=9.21ms; DR=70293/70293 ready=8.65ms; refusal frequency/check cost not root
INSTRUMENT_REPLY:131072 replies RR retry=0 retain=69.17ms send=791.50ms; DR retry=0 retain=19.35ms send=525.45ms; RR completion lane adds about 2us/op
ROOT_A_B:remove reply transport_generation_lock RR=29.556Kops/s,1.768ms; restored=27.654Kops/s,1.882ms; +6.9%; cacheline and atomic-reader alternatives did not explain/recover loss
PROFILE:perf unavailable (command missing, paranoid=2); callgrind RR 10-client/1s current=69.19M Ir/1205 ops, 0.15.1=69.04M Ir/1139 ops; instruction growth excluded, synchronization wall-time implicated
FIX_CANDIDATE:single-part completion validates connection generation under existing pipe out_sync; multipart keeps generation lock; teardown clears generation under peer out_sync
SMOKE_AFTER:noisy RR=28.574Kops/s,1.780ms DR=30.137Kops/s,1.769ms; direction confirmed; excluded from final table due external perf processes
TEST_BUILD:dev PASS JOBS=6; new stale-generation guarded-write test PASS
TEST_REPEAT:router_multiple_dealers and phase3_request_reply_contract 5/5 PASS; recovery tcp/tls/out-of-order and single-lane reply cases 5/5 PASS
TEST_FULL:140/141 PASS; test_single_lane_flow_snapshot_accounting timed out once at accounting wait, isolated run plus repeat 5/5 PASS; no reproducible failure
HOTPATH_GATE:PASS dealer_dealer=0.9947 dealer_router_reqrep=0.9965 pair=0.9999 router_router_tcp=0.9982
FINAL_017:RR=29.199Kops/s,1.765ms DR=29.960Kops/s,1.796ms; report=perf_c_multi_linux_20260905_024513.txt
FINAL_0151:RR=30.545Kops/s,1.032ms DR=29.313Kops/s,1.049ms; report=perf_c_multi_linux_20260905_024604.txt; ratios017/0151 RR=0.956 DR=1.022
FINAL_CHECK:release LTO lib PASS; git diff --check PASS; modified scope limited to core/src/** and core/tests/**
TEST_ZMP_TRANSACTION:5/5 PASS
EXIT:0
