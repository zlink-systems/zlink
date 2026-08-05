# Round 56 - STREAM/tls full-prefix failure focused repro

## 기준

- 목표 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 핵심 stream 목표:
  `MULTI_STREAM/tcp/64B = 400,124.6 ops/s`
- 현재 판단:
  round55 full repeat에서 `MULTI_STREAM/tcp/64B = 324,352.6 ops/s`로 목표 미달이다.
- failure gate:
  full multi failure 0을 먼저 만족해야 한다.

## 시작 상태

- core/perf source 변경 없음.
- round55에서 full sequence `MULTI_STREAM/tls` failure가 반복 재현되었다.
- round55 subset `SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM` `tcp,tls`는 success 48, fail 0으로
  failure를 재현하지 못했다.
- round55 full repeat는 success 174, fail 18, status partial이고
  `MULTI_STREAM/tls/256B`가 `non_zero_exit_2` 계열로 실패했다.

## 가설

1. failure는 STREAM 단독이 아니라 full sequence 앞쪽 workload 누적 후 TLS stream에서
   발생한다.
2. `non_zero_exit_2`는 stream client가 `case_metrics_t.pass == false`로 종료한 값이다.
   client stderr에 connect, send, recv, timeout, size mismatch 중 어느 조건인지 남길 수 있다.
3. `64B,256B`만 포함해도 full prefix를 거치면 같은 failure가 재현될 수 있다.

## 검증 예정

- focused full-prefix repro:
  - command:
    `PERF_DEBUG=1 PERF_DEBUG_TRANSITIONS=1 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64,256 bindings/c/perf/run_benchmarks_multi.sh --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round56_full_64_256_debug`
  - 목적:
    full sequence order를 유지하되 size를 `64B,256B`로 줄여 `STREAM/tls/256B` failure와
    client stderr를 재현한다.

## 검증 결과

- focused full-prefix debug repro:
  - command:
    `PERF_DEBUG=1 PERF_DEBUG_TRANSITIONS=1 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64,256 bindings/c/perf/run_benchmarks_multi.sh --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round56_full_64_256_debug`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_054839_round56_full_64_256_debug.txt`
  - outcome:
    중단.
  - 중단 위치:
    `MULTI_PUBSUB/wss/64B` 진행 중.
  - 중단 이유:
    `PERF_DEBUG=1`이 모든 앞쪽 pattern의 socket option/debug 출력을 크게 늘려
    `STREAM/tls` failure를 좁히는 재현 방식으로 부적절했다.
  - 정리:
    runner parent와 남은 `comp_src_pubsub_client` child process를 종료했다.

## 결과

- 이번 run은 `STREAM`까지 도달하지 못했으므로 `STREAM/tls` failure 판단 자료로 쓰지 않는다.
- `PERF_DEBUG=1`은 full-prefix 재현에 적합하지 않다. 필요한 경우 STREAM 전용 실행에서만
  사용하거나, runner report가 이미 제공하는 `non_zero_exit_2`와 source-level failure
  조건을 근거로 더 작은 재현을 만든다.

## 판정

- 목표 달성 여부: 미달성.
- source 변경 없음.
- 다음 단계는 `STREAM/tls`에 직접 초점을 맞춘 재현을 사용한다. full sequence 실패는
  round52와 round55에서 이미 반복 확인됐으므로, 이제 client return code 2를 만드는
  조건인 `connect`, `send`, `recv`, `timeout`, `size_mismatch`, `throughput==0` 중
  어느 쪽인지 더 좁혀야 한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 라운드는 source 변경 없이 재현과 로그 수집만 수행한다.
- WS/WSS pending message copy 제거, mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서,
  decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.
