# Round 16: 현재 LTO ON 64B targeted set 재측정

- goal: 현재 `core/build` 조건에서 64B 공통 항목의 남은 안정 결손을 다시 분리한다.
- 완료 기준: LTO ON `core/build`로 targeted 64B set을 측정하고, 문제 report 대비 평균/중앙값과 10% 이상 안정 결손 후보를 기록한다.
- 시작 시각: 2026-06-14 17:46:06 +0900
- 기준 commit: `812120e2b`
- 시작 git status: `bindings/javascript/samples/*` 변경이 있음. perf/core 작업과 무관하므로 건드리지 않는다. round 9-15 로그 파일이 untracked 상태다.
- 과거 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 최근 zero-fail report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_151925.txt`
- 대상 pattern/transport/size: 전체 multi pattern / `tcp,tls,ws,wss` / `64B`

## 가설

- 가설 1: round 14-15 이후에도 현재 runtime 기준으로 10% 이상 안정 결손이 남은 core hot path가 있다.
- 가설 2: 문제 report의 64B 하락 중 상당 부분은 실패, 측정 순서, perf 측정 의미 변화, 또는 단일 측정 노이즈에서 왔고, 현재 LTO ON 반복 측정으로는 core-only 수정 대상을 좁히기 어렵다.
- 선택한 가설: 먼저 가설 2를 검증한다. round 12-14에서 PUBSUB/SPOT tcp 하락은 반복 측정으로 사라졌고, round 15에서 장기 DEALER tcp 하락은 perf 측정 의미 변경 후보로 분리됐다.

## 읽을 코드와 조건

- `core/build/CMakeCache.txt`: `ENABLE_LTO=ON`, `CMAKE_BUILD_TYPE=Release`, `WITH_TLS=ON`인지 확인했다.
- `doc/plan/perf/core/core-library-performance-improvement-plan.ko.md`: 5% 미만은 오차, 10% 이상 반복 차이만 의미 있는 회귀/개선으로 본다.
- `bindings/c/perf/run_benchmarks_multi.sh`: `Perf runtime libzlink`가 `core/build` 아래인지 확인한 결과만 비교에 사용한다.

## 변경

- core 소스 변경: 없음
- perf 소스 변경: 없음
- 변경 이유: 이번 라운드는 수정 전 현재 반복 측정으로 남은 core-only 후보를 좁히는 측정 라운드다.
- perf 전용 변경이 아닌 이유: perf 코드는 수정하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.
- 추가로 실행한 회귀 테스트: 측정 전 `cmake --build core/build -j$(nproc)`를 실행한다.

## 검증 예정

- build:
  - `cmake --build core/build -j$(nproc)`
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM --transports tcp,tls,ws,wss --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round16_current_64b_lto`

## 결과

- build:
  - `cmake --build core/build -j$(nproc)` 통과
- targeted perf:
  - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM --transports tcp,tls,ws,wss --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round16_current_64b_lto`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - result: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_174702_round16_current_64b_lto.txt`
  - completion: success 31, fail 1, status partial
  - failure: `MULTI_STREAM current wss 64B: non_zero_exit_2_size_64`
- STREAM wss retry:
  - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports wss --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round16_stream_wss_retry`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - result: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_175959_round16_stream_wss_retry.txt`
  - completion: success 1, fail 0, status complete
  - median throughput: `180,880.2 msg/s`

## 비교

`round16_current_64b_lto` 결과에 `round16_stream_wss_retry`의 STREAM wss 값을 합쳐 비교했다.

- 문제 report 대비 공통 26개:
  - 평균: `-0.68%`
  - 중앙값: `-1.10%`
  - 최소: `-10.98%`
  - 최대: `+11.09%`
- 문제 report 대비 one-way 10개:
  - 평균: `-4.00%`
  - 중앙값: `-4.25%`
  - 최소: `-10.98%`
  - 최대: `+11.09%`
- 문제 report 대비 echo 16개:
  - 평균: `+1.40%`
  - 중앙값: `+1.65%`
  - 최소: `-7.11%`
  - 최대: `+8.34%`

문제 report 대비 하락 폭이 큰 항목은 아래와 같다.

| pattern | transport | 변화 |
|---------|-----------|------|
| `MULTI_PUBSUB` | ws | `-10.98%` |
| `MULTI_PUBSUB` | tls | `-8.36%` |
| `MULTI_PUBSUB` | tcp | `-8.19%` |
| `MULTI_STREAM` | tcp | `-7.11%` |
| `MULTI_DEALER_DEALER` | ws | `-5.70%` |
| `MULTI_PUBSUB` | wss | `-5.27%` |

## 판정

- 전체 64B 공통 항목은 문제 report 대비 `+10%` 목표에 도달하지 못했다. 다만 현재 반복 측정에서는 목표로 삼을 만큼 큰 core-only 결손도 대부분 사라졌다.
- 10% 이상 남은 항목은 `MULTI_PUBSUB ws` 하나뿐이다.
- `MULTI_PUBSUB`는 round 12-14에서도 측정 순서와 반복에 따라 크게 흔들렸으므로, full targeted set 결과만으로 core 변경 후보로 확정하지 않는다.
- 다음 단계는 `MULTI_PUBSUB`만 단독 반복해 ws 하락이 재현되는지 확인한다.

## PUBSUB 단독 재확인

- command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tcp,tls,ws,wss --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round16_pubsub_repeat`
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- result: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_180146_round16_pubsub_repeat.txt`
- completion: success 4, fail 0, status complete

문제 report 대비 PUBSUB 단독 반복 결과는 아래와 같다.

| transport | 변화 |
|-----------|------|
| tcp | `-3.88%` |
| tls | `-5.80%` |
| ws | `-5.86%` |
| wss | `-5.24%` |

`round16_current_64b_lto`, STREAM wss retry, PUBSUB repeat를 합치면 문제 report 대비 공통 26개는 아래와 같다.

- 평균: `-0.21%`
- 중앙값: `-1.10%`
- one-way 평균: `-2.79%`
- echo 평균: `+1.40%`
- 최악 항목: `MULTI_STREAM tcp -7.11%`

## 최종 판정

- 이 라운드에서는 문제 report 대비 10% 이상 반복되는 성능 결손을 확인하지 못했다.
- full targeted set에서 보인 `MULTI_PUBSUB ws -10.98%`는 PUBSUB 단독 반복에서 `-5.86%`로 내려가 10% 임계값을 만족하지 않았다.
- `MULTI_STREAM wss` 실패는 단독 재실행에서 success 1, fail 0으로 통과해 일시 실패로 분리한다.
- 따라서 이 라운드에서는 core 소스 변경을 하지 않는다. 5% 안팎 변동에 맞춘 변경은 계획의 측정 원칙에 맞지 않는다.
- 다음 후보는 문제 report 대비가 아니라 과거 기준 대비 큰 하락 항목을 별도 감사하는 것이다. 다만 round 15의 DEALER처럼 perf 측정 의미 변경 가능성을 먼저 배제해야 한다.
