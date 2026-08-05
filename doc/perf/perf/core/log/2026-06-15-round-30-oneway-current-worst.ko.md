# Round 30: 64B one-way current worst 재선정

- 목표: round29 clean full 64B sweep 기준으로 아직 남은 current problem 대비 회귀 항목을 재선정하고, perf 전용 우회 없이 core runtime hot path 후보만 검증한다.
- 기준 baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 clean sweep: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_235057_round29_current_64b_sweep.txt`
- 시작 git status: core source diff 없음. unrelated .NET doc 및 기존 perf log untracked는 무시한다.

## 병목 가설

1. one-way 경로의 공통 pipe/write/read activation 비용이 현재 64B 처리량 하락에 남아 있다.
2. PUBSUB 또는 DEALER_DEALER의 transport별 편차는 core hot path가 아니라 측정 순서/load/benchmark-side 변화일 수 있으므로 반복 측정으로 10% 이상 결손인지 먼저 확인해야 한다.
3. STREAM/tcp 64B는 400k baseline 대비 낮지만, round26~29에서 core 후보가 모두 실패했고 perf helper `send_mutex` 영향이 큰 것으로 분리되었으므로 이번 첫 후보로 삼지 않는다.

## 먼저 검증할 가설

- round29 clean sweep에서 problem report 대비 5% 이상 낮은 one-way 항목을 반복 측정한다.
- 반복에서도 10% 이상 결손이 재현되는 항목만 core call path를 추적한다.

## 반복 측정 결과

command:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern SPOT,PUBSUB,DEALER_DEALER \
  --transports tcp,tls,ws,wss \
  --duration 5 \
  --runs 3 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round30_oneway_worst_repeat
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_002958_round30_oneway_worst_repeat.txt`
- completion: success 12, fail 0, status complete

요약:
- `MULTI_SPOT tcp 64B`: 3,716,100.0 ops/s, problem 대비 -4.62%. 10% 반복 결손 아님.
- `MULTI_SPOT tls 64B`: 3,526,864.6 ops/s, problem 대비 -5.67%. 10% 반복 결손 아님.
- `MULTI_DEALER_DEALER tcp/ws/wss 64B`: problem 대비 약 -4.6%/-5.6%/-4.9%. 10% 반복 결손 아님.
- `MULTI_PUBSUB tcp 64B`: 2,349,093.2 ops/s, problem 대비 -10.62%. 반복 결손 후보.
- `MULTI_PUBSUB tls/ws/wss 64B`: problem 대비 -7.48%/-3.33%/-5.23%. 10% 반복 결손 아님.

판정:
- 이번 round의 core 후보는 `MULTI_PUBSUB tcp 64B`로 좁힌다.
- SPOT, DEALER_DEALER는 반복 기준에서 10% 이상 결손이 아니므로 이번 라운드에서 수정하지 않는다.

## PUBSUB/tcp call path and candidate

- Current repeated gap source: `perf_c_multi_linux_20260615_002958_round30_oneway_worst_repeat.txt` shows `MULTI_PUBSUB/tcp/64 = 2,349,093.2`, problem `2,628,324.0`, delta `-10.62%`.
- Server path: `perf_zlink_publish_parts(server, "bench", &payload_part, 1, ZLINK_DONTWAIT)` -> `zlink_publish_part()` -> generic publish part helper -> `send_socket_part_publish_impl()` sends topic frame with `ZLINK_SNDMORE`, then payload final.
- Core path: `xpub_t::xsend()` matches subscribers from `_subscriptions` on the topic frame, sends through `dist_t`, then final payload clears matching via `_dist.unmatch()`.
- Excluded repeats: round8 distributor match index lookup, round9/round28 publish single-final fast path, and round11 distributor final write helper already failed or mixed.
- Candidate: preserve the distributor matching prefix for repeated identical first frames while subscription/pipe topology is unchanged. Do not reuse HWM readiness; invalidate HWM cache after every completed message so backpressure is still checked per message.

## Candidate A result: XPUB repeated-topic matching cache

- Build: `cmake --build core/build -j$(nproc)` passed.
- Test: `ctest --test-dir core/build --output-on-failure -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|transport_matrix|multi_socket_contract_regressions|spot_pubsub_scenario)$|unittest_mtrie'` passed 7/7.
- Perf command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round30_pubsub_tcp_matching_cache`
- Report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_004349_round30_pubsub_tcp_matching_cache.txt`
- Result: `MULTI_PUBSUB/tcp/64 = 2,450,926.8`.
- Against round30 repeat `2,349,093.2`: `+4.34%`.
- Against problem `2,628,324.0`: `-6.75%`.
- Against round29 full-sweep current `2,497,982.3`: `-1.88%`.
- Decision: not a clear source win. Revert candidate.

## STREAM/tcp baseline-target correction

- User-corrected baseline target: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`, `MULTI_STREAM/tcp/64 = 400,124.6`.
- Problem report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`, `MULTI_STREAM/tcp/64 = 299,395.0`.
- Clean source recheck command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round30_stream_tcp_clean_recheck`
- Report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_004607_round30_stream_tcp_clean_recheck.txt`
- Runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- Result: `323,970.6`, baseline delta `-19.03%`, problem delta `+8.21%`.
- Note: latest clean run does not reproduce 250k, but still misses the 400k target. Continue STREAM/tcp core-path selection.

## Candidate B diagnostic: remove STREAM perf send mutex

- Rationale: current perf helper serialized STREAM echo sends with `send_mutex`, while current core documents and tests thread-safe `send`/`publish`/`send_rid`; baseline did not have this extra mutex. This is a benchmark-side diagnostic only.
- Change: remove `session_t::send_mutex` and the two `lock_guard` uses from `bindings/c/perf/multi/common/perf_multi_stream_session.hpp`.
- Contract tests: `ctest --test-dir core/build --output-on-failure -R 'test_(stream_threadsafe|stream_fastpath|stream_socket|stream_send_blocking_wakeup|multi_stream_server_reassembly|thread_safe_contract_policy)$'` passed 6/6.
- Perf runner rebuild: `cmake --build bindings/c/build --target comp_src_stream_server -j$(nproc)` passed.
- Perf command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round30_stream_tcp_no_send_mutex_candidate`
- Report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_004830_round30_stream_tcp_no_send_mutex_candidate.txt`
- Result: `MULTI_STREAM/tcp/64 = 375,799.2`.
- Against clean recheck `323,970.6`: `+16.00%`.
- Against problem `299,395.0`: `+25.52%`.
- Against baseline target `400,124.6`: `-6.08%`.
- Decision: do not keep. The active perf plan forbids perf client/server speedups as a core performance improvement. Revert after using the result to explain part of the STREAM gap.

## Candidate B baseline-option check

- Baseline file options include `connect_concurrency: 128 (default)`, while current runner default showed `1024`.
- Recheck command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-concurrency 128 --connect-ready-timeout-ms 5000 --results-tag round30_stream_tcp_no_mutex_connect128`
- Report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_004905_round30_stream_tcp_no_mutex_connect128.txt`
- Result: `383,141.6`, baseline delta `-4.25%`.

## Candidate B extra probes

- `PERF_DISABLE_RESOURCE_METRICS=1` with no mutex/connect128: `379,824.2`; no improvement.
- IO 5/5 with no mutex/connect128: `346,632.8`; worse.
- IO 6/6 with no mutex/connect128: `367,045.4`; worse.
- Baseline-shaped runs=1/connect128 with no mutex: `375,821.8`; does not cross 400k.
- Decision: still do not keep mutex removal. The probes only show that the perf helper serialization explains a large part of the STREAM gap; they are not core runtime hot-path improvements.
