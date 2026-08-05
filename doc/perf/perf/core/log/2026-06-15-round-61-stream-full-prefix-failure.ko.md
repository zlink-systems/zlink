# Round 61: STREAM full-prefix failure 원인 추적

- goal: full multi perf 실패 0개 목표를 위해 full-prefix 뒤 `MULTI_STREAM`에서 발생하는
  `non_zero_exit_2` 실패의 원인을 core/runtime 관점에서 분리한다.
- 완료 기준:
  - full-prefix 축소 재현에서 실패 조건을 pass 조건 항목까지 좁힌다.
  - core source 버그가 확인되면 최소 수정 후 build/test/targeted perf를 실행한다.
  - perf runner/client/server 성능 변경은 하지 않는다.
- 시작 시각: 2026-06-15 KST
- 시작 git status:
  - core/perf source diff 없음.
  - dotnet 문서 변경은 이 작업과 무관하므로 건드리지 않는다.
- 기준 report:
  - baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - problem: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 최근 실패 report:
  - round55: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_051558_round55_full_repeat_after_subset.txt`
  - round57: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_055424_round57_full_64_256_repro.txt`

## 가설

- 가설 1: full-prefix 이후 STREAM 클라이언트 일부 connection이 connect-ready window를 만족하지 못해
  `connect_ok < required_connect` 또는 `timeout_error`로 return code 2가 발생한다.
- 가설 2: STREAM 서버/transport core 경로가 full-prefix 이후 특정 transport 전환에서 pipe/engine
  종료 또는 backpressure 상태를 잘못 남겨 `send_error` 또는 `recv_error`가 발생한다.
- 선택한 가설: 먼저 실패한 client pass 조건을 확인한다. source 수정은 실패 항목이 core 쪽으로 좁혀진 뒤 한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 아직 없음.
- 보안 의미를 유지한 근거: 현재는 측정/원인 추적 단계이며 source 변경 없음.
- 추가로 실행한 회귀 테스트: source 후보가 생기면 기록한다.

## Debug full-prefix 재현 시도

- command:
  `PERF_DEBUG=1 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64,256 PERF_CAPTURE_MAX_BYTES=16777216 bindings/c/perf/run_benchmarks_multi.sh --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round61_full_64_256_debug_no_transitions`
- runner runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_061741_round61_full_64_256_debug_no_transitions.txt`
- completion:
  - success: `64`
  - fail: `0`
  - status: `complete`
- STREAM 결과:
  - `tcp/64B = 328,767.0 ops/s`
  - `tcp/256B = 341,675.6 ops/s`
  - `tls/64B = 231,131.0 ops/s`
  - `tls/256B = 217,173.8 ops/s`
  - `ws/64B = 293,275.4 ops/s`
  - `ws/256B = 280,910.4 ops/s`
  - `wss/64B = 191,429.4 ops/s`
  - `wss/256B = 186,315.2 ops/s`

## 중간 판정

- 같은 `64B,256B` full-prefix 조건에서 `PERF_DEBUG=1` run은 실패를 재현하지 못했다.
- 이는 두 가능성을 남긴다.
  1. `PERF_DEBUG=1` 또는 낮은 시작 부하가 timing을 바꿔 failure를 숨겼다.
  2. round55/57 failure가 반복성은 있지만 매 run 재현되는 deterministic core failure는 아니다.
- 다음 확인은 debug 없이 같은 조건을 다시 반복한다.

## No-debug full-prefix 반복

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64,256 bindings/c/perf/run_benchmarks_multi.sh --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round61_full_64_256_nodebug_repeat`
- runner runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_062935_round61_full_64_256_nodebug_repeat.txt`
- completion:
  - success: `64`
  - fail: `0`
  - status: `complete`
- STREAM 결과:
  - `tcp/64B = 341,431.4 ops/s`
  - `tcp/256B = 349,203.4 ops/s`
  - `tls/64B = 227,686.2 ops/s`
  - `tls/256B = 218,322.4 ops/s`
  - `ws/64B = 282,713.6 ops/s`
  - `ws/256B = 285,970.6 ops/s`
  - `wss/64B = 189,912.8 ops/s`
  - `wss/256B = 184,809.2 ops/s`

## 반복 후 판정

- `64B,256B` full-prefix 조건은 debug 1회와 no-debug 1회에서 모두 `fail 0`으로 완료됐다.
- round55/57의 `non_zero_exit_2` 실패는 현재 조건에서 deterministic하게 재현되지 않는다.
- source 변경 근거가 아직 없으므로 core 코드는 수정하지 않는다.
- 다음 확인은 전체 message size full run으로 failure gate를 다시 판단한다.

## Full all-size failure gate

- command:
  `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round61_full_allsize_failure_gate`
- runner runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_064234_round61_full_allsize_failure_gate.txt`
- completion:
  - success: `54`
  - fail: `18`
  - status: `partial`
- failure:
  - `MULTI_ROUTER_ROUTER current tls 131072B`
  - reason: `non_zero_exit_-6_malloc_consolidate(): unaligned fastbin chunk detected_size_65536`

## ROUTER_ROUTER/tls focused 재현

- command:
  `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --pattern ROUTER_ROUTER --transports tls --msg-sizes 65536,131072 --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round61_rr_tls_large_focused`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_065120_round61_rr_tls_large_focused.txt`
- completion:
  - success: `2`
  - fail: `0`
  - status: `complete`
- result:
  - `MULTI_ROUTER_ROUTER/tls/65536B = 53,342.6 ops/s`
  - `MULTI_ROUTER_ROUTER/tls/131072B = 25,987.6 ops/s`

## Prefix focused 재현

- large-only prefix command:
  `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER --transports tcp,tls --msg-sizes 65536,131072 --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round61_rr_tls_large_prefix_focused`
- large-only prefix report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_065147_round61_rr_tls_large_prefix_focused.txt`
- large-only prefix completion:
  - success: `12`
  - fail: `0`
  - status: `complete`
- all-size prefix command:
  `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER --transports tcp,tls --msg-sizes 64,256,1024,4096,65536,131072 --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round61_rr_tls_allsize_prefix_repro`
- all-size prefix report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_065345_round61_rr_tls_allsize_prefix_repro.txt`
- all-size prefix completion:
  - success: `36`
  - fail: `0`
  - status: `complete`

## Round 61 판정

- full all-size run에서 heap abort가 1회 재현되었지만, 실패 지점 단독과 실패 직전 prefix 축소 조건에서는
  재현되지 않았다.
- `malloc_consolidate(): unaligned fastbin chunk detected`는 메모리 손상 신호이므로 무시하면 안 된다.
- 다만 현재 증거만으로 core source의 특정 변경 지점을 좁힐 수 없고, source 수정은 근거 없는 추측이 된다.
- 이 round에서는 core/perf source를 수정하지 않는다.
- 다음 round는 사용자가 정정한 기준인 `baseline perf_c_multi_linux_20260513_101034.txt`의
  `MULTI_STREAM/tcp/64B = 400,124.6 ops/s`를 기준으로 stream/tcp/64B 회귀를 다시 추적한다.
