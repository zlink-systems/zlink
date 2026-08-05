# Round 40: one-way 64B fanout and pipe path

- goal: core 64B one-way hot path 회귀를 줄인다.
- 완료 기준: 문제 report 대비 one-way 64B 평균 `+10%` 이상, 전체 64B 중앙값 `+10%`
  이상 또는 평균 `+8%` 이상, 관련 core tests 통과, 작업 로그 작성.
- 시작 시각: 2026-06-15 02:26 KST
- 기준 commit: `72d893595`
- 시작 git status:
  - core/perf source diff: empty
  - unrelated dirty tree: `framework/languages/dotnet/doc/...`, `_workspace/`
  - untracked perf logs exist under `doc/plan/perf/core/log/`
- 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 실패 0개 64B sweep:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_235057_round29_current_64b_sweep.txt`

## 64B 비교

- 문제 report는 `success 152`, `fail 40`이다.
- round29 current 64B sweep은 `success 32`, `fail 0`이다.
- baseline vs problem common 64B:
  - n=26
  - mean `-15.62%`
  - median `-14.86%`
  - `<= -10%`: 18개
- current64 vs problem common 64B:
  - n=26
  - mean `+0.91%`
  - median `+1.12%`
  - `>= +8%`: 6개
  - `>= +10%`: 2개
  - `<= -10%`: 1개
- current64 vs problem one-way:
  - n=10
  - mean `-5.78%`
  - median `-5.31%`
  - `>= +10%`: 0개
  - `<= -10%`: 1개
- current64 vs problem echo:
  - n=16
  - mean `+5.10%`
  - median `+4.05%`

## 큰 하락 항목

current64 vs problem one-way worst:

- `MULTI_SPOT/tcp/64`: `-15.91%` (`3896078.6` -> `3276035.2`)
- `MULTI_SPOT/tls/64`: `-9.35%` (`3739003.6` -> `3389574.4`)
- `MULTI_PUBSUB/tls/64`: `-7.12%` (`2446707.8` -> `2272469.6`)
- `MULTI_DEALER_DEALER/ws/64`: `-5.62%` (`3156838.0` -> `2979277.4`)
- `MULTI_DEALER_DEALER/tcp/64`: `-5.33%` (`3045747.2` -> `2883333.2`)

current64 vs baseline one-way worst:

- `MULTI_SPOT/tcp/64`: `-55.61%`
- `MULTI_SPOT/wss/64`: `-53.69%`
- `MULTI_SPOT/tls/64`: `-51.05%`
- `MULTI_SPOT/ws/64`: `-50.17%`
- `MULTI_PUBSUB/tls/64`: `-31.83%`

## 가설

- 가설 1: SPOT/PUBSUB fanout에서 한 메시지를 여러 pipe로 보낼 때, per-target 복사나
  poller/ready 갱신이 64B one-way hot path에 남아 있다. 모든 target이 즉시 성공하는
  steady state에서 불필요한 bookkeeping을 줄이면 SPOT/PUBSUB가 함께 오른다.
- 가설 2: pipe enqueue/write path에서 final single-part 메시지의 HWM/flush/activate
  처리가 one-way pattern 전반에 공통 비용이 된다. 단, blocking send wakeup과 LWM
  계약을 깨면 안 된다.
- 가설 3: mailbox/wakeup 또는 ASIO poller에서 one-way send readiness 갱신이 작은
  메시지에서 과도하게 발생한다. 하지만 이전 direct activate/read-drain 후보들은 실패했으므로
  새 근거가 필요하다.

## 선택한 가설

- 먼저 가설 1을 검증한다.
- 이유: 현재 미달성은 stream보다 one-way 평균이며, baseline 대비 가장 큰 하락이 SPOT
  one-way다. SPOT/PUBSUB fanout은 pipe write hot path와도 맞닿아 있어 전체 one-way 평균에
  더 직접적이다.

## 기준 정정 및 중단

- 사용자 정정: 이번 목표의 기준은 `MULTI_STREAM/tcp/64B`이며, 목표는 baseline report의
  `400,124.6 ops/s` 회복이다.
- baseline report:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 잘못 시작한 one-way clean baseline 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,PUBSUB,SPOT --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round40_oneway_clean_baseline`
- 이 명령은 source 변경 없이 실행했지만 목표 축과 달라 `MULTI_SPOT/tcp` 도중 종료했다.
- 확인된 일부 결과:
  - `MULTI_DEALER_DEALER/tcp/64`: `2,900,865.0 ops/s`
  - `MULTI_DEALER_DEALER/tls/64`: `2,995,869.0 ops/s`
  - `MULTI_DEALER_DEALER/ws/64`: `2,982,807.0 ops/s`
  - `MULTI_DEALER_DEALER/wss/64`: `3,124,633.0 ops/s`
  - `MULTI_PUBSUB/tcp/64`: `2,487,201.0 ops/s`
  - `MULTI_PUBSUB/tls/64`: `2,300,968.0 ops/s`
  - `MULTI_PUBSUB/ws/64`: `2,203,715.0 ops/s`
  - `MULTI_PUBSUB/wss/64`: `2,527,414.0 ops/s`
- 다음 작업은 `STREAM/tcp/64B` 단독 반복과 core stream path 후보 검증으로 되돌린다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: Pending.
- 보안 의미를 유지한 근거: Pending.
- 추가로 실행한 회귀 테스트: Pending.
