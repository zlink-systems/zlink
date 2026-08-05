# Round 23: evidence 정리와 다음 core 후보 선정

- goal: round 15-22 결과를 정리해 현재 반복 기준에서 source 변경 후보가 남았는지 판정한다.
- 완료 기준: 문제 report 대비 반복 10% gap, 실패 상태, 과거 기준 장기 gap, 이미 실패한 후보를 분리하고, 다음 source 후보가 있으면 call path를 읽는다. 후보가 없으면 core 변경을 보류한다.
- 시작 시각: 2026-06-14 18:43:31 +0900
- 기준 commit: `84e10b266`
- 시작 git status: `bindings/java/src/main/resources/native/linux-x86_64/libzlink.so.6.0.4`, `bindings/java/src/test/java/systems/zlink/contract/OptimizationGuardContractTest.java` 변경이 있음. perf/core 작업과 무관하므로 건드리지 않는다. round 9-22 로그 파일이 untracked 상태다.
- 과거 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 주요 현재 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_151925.txt`
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_174702_round16_current_64b_lto.txt`
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_183228_round21_current_64b_sweep.txt`
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_184023_round22_spot_tcp_current_head_repeat.txt`

## 가설

- 가설 1: 현재 반복 기준에서 문제 report 대비 10% 이상 gap은 없다. 남은 큰 장기 하락은 perf 측정 의미 변화와 run-to-run variance가 섞여 있어 바로 source 후보가 아니다.
- 가설 2: 문제 report 대비 10% gap은 없지만, 과거 기준 대비 one-way 하락이 큰 공통 core 경로가 아직 있고, source 변경 전 read-only call-path 추적으로 후보를 하나 좁힐 수 있다.
- 선택한 가설: 먼저 가설 1을 검증하고, 그 뒤 가설 2에 해당하는 후보가 있는지 이미 실패한 round와 겹치지 않게 확인한다.

## 변경

- core 소스 변경: 없음
- perf 소스 변경: 없음
- 변경 이유: source 후보 선정 전 evidence를 정리한다.
- perf 전용 변경이 아닌 이유: perf 코드는 수정하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: 분석 라운드이며 WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.
- 추가로 실행한 회귀 테스트: 소스 변경이 없으면 별도 test는 실행하지 않는다.

## 결과

- repeated evidence 병합 기준:
  - 기준: round 16 current targeted set
  - 대체/보강: round 16 PUBSUB repeat, round 16 STREAM wss retry, round 18 STREAM tcp repeat, round 22 SPOT tcp repeat
- 문제 report 대비 repeated evidence:
  - 공통 64B 26개 평균 `-0.09%`, 중앙값 `-0.81%`
  - one-way 64B 14개 평균 `-2.28%`, 중앙값 `-3.39%`
  - echo 64B 12개 평균 `+2.47%`, 중앙값 `+1.94%`
  - 최저 항목: `MULTI_SPOT tcp 64B` `-8.81%`
  - 문제 report 대비 반복 10% 이상 결손: 없음
- 과거 기준 대비 repeated evidence:
  - 전체 64B 32개 평균 `-13.89%`, 중앙값 `-11.44%`
  - one-way 64B 16개 평균 `-30.33%`, 중앙값 `-25.86%`
  - echo 64B 16개 평균 `+2.55%`, 중앙값 `-5.56%`
  - 최저 항목: `MULTI_SPOT tcp 64B` `-51.86%`
- round 21 single sweep는 문제 report 대비 `MULTI_SPOT tcp 64B`가 `-14.90%`였지만, round 22 standalone repeat에서 `-8.81%`로 올라와 10% 기준을 만족하지 않았다.

## 이미 제외한 source 후보

- SPOT publish ingress 직접 forward: round 3에서 반복 개선 없음.
- send monitor counter 제거: round 4에서 효과 없음. monitor blocked ratio 계약 때문에 남길 수 없음.
- ASIO output batch hot path: round 5에서 효과 없음.
- pipe single flush flags: round 6에서 효과 없음.
- SPOT per-message pump 제거: round 7에서 효과 없음.
- distributor matching index / multipart publish frame: round 8에서 효과 없음.
- `zlink_publish_part()` single-final fast path: round 9에서 효과 없음.
- small LMSG block pool: round 10에서 mixed/worse.
- distributor final data helper: round 11에서 transport별 혼재와 악화.
- load-balancer one-active final helper: round 19에서 `DEALER_DEALER tcp 64B` `-11.56%`.
- SPOT owned frame consume/vector류:
  - consume은 pending fallback과 local/mesh 재사용 조건이 좁아 round 7에서 바로 변경하지 않기로 했다.
  - `spot_owned_msg_parts_t`를 단순히 `std::vector`로 바꾸는 후보는 `zlink_msg_t`가 명시적 move/close를 요구하는 owning object라서 reallocation bit-move 위험이 있다. 모든 append path를 완전히 reserve하지 않는 한 안전한 작은 변경이 아니다.

## 판정

- 현재 문제 report 대비 반복 10% 이상 결손은 없다.
- 과거 기준 대비 one-way 장기 하락은 크지만, round 15/17에서 확인한 perf client/server 측정 의미 변화와 round 20/22의 SPOT variance 때문에 바로 core source 회귀로 확정할 수 없다.
- 현재 기준에서 source 변경을 정당화할 후보는 없다. source 후보가 되려면 같은 runner 조건에서 반복 10% 이상 개선 또는 결손 재현이 먼저 필요하다.
- core 소스 변경 없음. perf 소스 변경 없음.
- 보안 하드닝 보호 항목은 건드리지 않았다.

## 다음 작업

- current HEAD에서 full multi failure 0 상태를 다시 확인한다. round 21은 64B sweep라서 전체 크기/패턴 failure 0 증거가 아니다.
- full multi에서 실패가 나오면 다음 라운드는 실패 안정화로 전환한다.
- full multi가 failure 0이면, 현재 source 후보 부재 상태를 유지하고 장기 하락은 perf 측정 의미 감사와 분리한다.

## full multi failure gate

- command:
  - `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --connect-ready-timeout-ms 5000 --results-tag round23_full_failure_gate`
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- `META,commit`: `84e10b266`
- load_avg: `2.84 2.54 3.26`
- 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_184516_round23_full_failure_gate.txt`
- completion: success 180, fail 12, status partial
- fail-fast stop:
  - `MULTI_STREAM current ws 1024B: non_zero_exit_2_size_1024`
  - result line 누락: bandwidth, latency, latency_p95, latency_p99, throughput

## round 23 최종 판정

- current HEAD full multi failure 0 상태가 아니다.
- 성능 개선보다 실패 안정화가 우선이다.
- 다음 라운드는 `MULTI_STREAM ws 1024B` 실패를 단독 반복해 재현성을 확인하고, core STREAM/WS 경로 또는 perf 측정 실패인지 분리한다.
- core 소스 변경 없음. perf 소스 변경 없음.
