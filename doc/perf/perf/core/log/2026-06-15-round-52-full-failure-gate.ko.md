# Round 52: full multi 실패 0 gate

- 목표: 현재 checkout에서 full multi failure가 0인지 확인한다. 실패가 재현되면
  성능 최적화보다 먼저 실패 경로를 core 기준으로 분리한다.
- 완료 기준:
  - full multi perf를 source 변경 없이 실행한다.
  - 실패가 있으면 pattern/transport/size와 에러를 기록하고, runner 조건 변경 없이
    core 문제인지 분리한다.
  - 실패가 없으면 성능 작업은 `MULTI_STREAM/tcp/64B`와 echo 계열의 구조 병목으로
    돌아간다.
- 시작 시각: 2026-06-15 KST
- 시작 git status:
  - core/perf source diff: empty
  - perf log files under `doc/plan/perf/core/log` are untracked
- 기준 report:
  - baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - problem: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
  - latest one-way repeat:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_041202_round51_oneway_64b_repeat.txt`

## 기준 수치

- baseline completion: success 184, fail 0.
- problem completion: success 152, fail 40.
- baseline `MULTI_STREAM/tcp/64B`: `400,124.6 ops/s`.
- problem `MULTI_STREAM/tcp/64B`: `299,395.0 ops/s`.
- round49 rebuilt `MULTI_STREAM/tcp/64B`: `326,046.4 ops/s`.
- round51 one-way 64B completion: success 12, fail 0.

## 가설

1. 현재 checkout에서는 full multi failure 0이 유지된다. problem report의 40개 실패는
   이전 source 상태 또는 실행 순서와 부하 조건의 결과다.
2. `STREAM`, `SPOT_REQREP`, `SPOT_SENDSEND`의 `ws/wss` 계열 실패가 full sequence에서
   아직 재현될 수 있다. 재현되면 perf runner를 바꾸지 않고 core 경로를 먼저 본다.
3. full pass가 확인되면 남은 문제는 실패가 아니라 `MULTI_STREAM/tcp/64B`의
   400k target 미달이다. 다음 라운드는 one-way 변경이 아니라 STREAM/echo 구조
   병목을 대상으로 한다.

선택한 확인: source 변경 없이 full multi perf를 1회 실행한다. 이전 perf-helper 임시
변경으로 `--reuse-build` 측정이 오염된 적이 있으므로 perf binary를 재빌드하는 기본
경로를 사용한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거: 이번 라운드는 source 변경 없이 측정만 수행한다. WS/WSS
  pending message copy 제거, mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서,
  decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.

## 검증

- full perf:
  - command:
    `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round52_full_failure_gate`
  - runner runtime:
    `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_042257_round52_full_failure_gate.txt`
  - completion: success 174, fail 18, status partial.
  - elapsed: 29m 27s.

## 결과

- `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`,
  `MULTI_PUBSUB`, `MULTI_SPOT`, `MULTI_SPOT_REQREP`, `MULTI_SPOT_SENDSEND`는
  full sequence에서 모두 성공했다.
- `MULTI_STREAM/tcp`는 6개 size 모두 성공했다.
- `MULTI_STREAM/tls`는 `64B`부터 실패했고 report의 failure 목록은 아래 6개다.
  - `MULTI_STREAM current tls 64B: non_zero_exit_2_size_64`
  - `MULTI_STREAM current tls 256B: non_zero_exit_2_size_64`
  - `MULTI_STREAM current tls 1024B: non_zero_exit_2_size_64`
  - `MULTI_STREAM current tls 4096B: non_zero_exit_2_size_64`
  - `MULTI_STREAM current tls 65536B: non_zero_exit_2_size_64`
  - `MULTI_STREAM current tls 131072B: non_zero_exit_2_size_64`
- completion의 fail 수는 18이지만 failure 목록에는 `tls` 6개만 있다. `tls` 실패 뒤
  `STREAM/ws,wss`가 실행되지 않아 expected result line 대비 부족분까지 fail로 센
  것으로 보인다.

## STREAM 기준 판정

| item | value |
|------|-------|
| baseline `MULTI_STREAM/tcp/64B` | `400,124.6 ops/s` |
| problem `MULTI_STREAM/tcp/64B` | `299,395.0 ops/s` |
| round49 rebuilt `MULTI_STREAM/tcp/64B` | `326,046.4 ops/s` |
| round52 full `MULTI_STREAM/tcp/64B` | `330,345.4 ops/s` |

- round52 full의 `MULTI_STREAM/tcp/64B`는 baseline 대비 `-17.44%`다.
- problem 대비로는 `+10.34%`지만 목표인 400kops에는 아직 미달한다.
- 사용자가 지적한 기준은 `ws`가 아니라 baseline의 `STREAM/tcp/64B` 400kops다.
  이후 STREAM 작업은 이 항목을 기준으로 삼는다.

## 판정

- 목표 달성 여부: 미달성.
- full failure 0 조건도 아직 미달성이다. 다만 실패는 전체에 퍼져 있지 않고
  `MULTI_STREAM/tls` 시작 지점으로 좁혀졌다.
- 다음 확인은 source 변경 없이 `MULTI_STREAM/tls/64B` 단독 재현을 실행해서, full
  sequence 이후의 누적 상태 문제인지 STREAM tls 자체 문제인지 분리한다.

## 추가 재현 확인

- `STREAM/tls/64B` 단독:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports tls --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round52_stream_tls64_repro`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_045259_round52_stream_tls64_repro.txt`
  - result: success 1, fail 0, status complete.
  - `MULTI_STREAM/tls/64B`: `228,244.6 ops/s`.
- `STREAM tcp,tls` 순서:
  - command:
    `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports tcp,tls --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round52_stream_tcp_then_tls_repro`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_045320_round52_stream_tcp_then_tls_repro.txt`
  - result: success 12, fail 0, status complete.
  - `MULTI_STREAM/tcp/64B`: `323,793.8 ops/s`.
  - `MULTI_STREAM/tls/64B`: `226,287.8 ops/s`.

추가 재현에서는 `STREAM/tls` 자체나 `STREAM/tcp` 직후 순서가 실패하지 않았다. 따라서
round52 full 실패는 현재 확인만으로는 반복 가능한 core 기능 실패로 확정하지 않는다.
다음 round에서는 `MULTI_STREAM/tcp/64B` 400kops 미달을 직접 대상으로 본다.
