# Round 22: current HEAD SPOT tcp 64B 반복 확인

- goal: round 21 sweep에서 문제 report 대비 `-14.90%`로 나온 `MULTI_SPOT tcp 64B`가 현재 HEAD에서도 standalone 반복 결손인지 확인한다.
- 완료 기준: current HEAD `core/build`로 `MULTI_SPOT tcp 64B`를 5-run 반복 측정한다. 문제 report 대비 10% 이상 결손이면 SPOT data-plane source 후보를 추적하고, 아니면 source 변경을 보류한다.
- 시작 시각: 2026-06-14 18:39:51 +0900
- 기준 commit: `84e10b266`
- 시작 git status: `bindings/java/src/test/java/systems/zlink/contract/OptimizationGuardContractTest.java` 변경이 있음. perf/core 작업과 무관하므로 건드리지 않는다. round 9-21 로그 파일이 untracked 상태다.
- 과거 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- current sweep report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_183228_round21_current_64b_sweep.txt`
- 대상 pattern/transport/size: `MULTI_SPOT` / `tcp` / `64B`

## 현재 수치 요약

- 문제 report `MULTI_SPOT tcp 64B`: `3,896,078.6 msg/s`
- round 20 standalone repeat: `3,686,940.0 msg/s`, 문제 report 대비 `-5.37%`
- round 21 current sweep: `3,315,733.4 msg/s`, 문제 report 대비 `-14.90%`
- 과거 기준: `7,379,815.4 msg/s`

## 가설

- 가설 1: current HEAD에서도 standalone repeat가 10% 이상 낮게 재현되어 SPOT data-plane publish ingress/local fanout 경로가 source 후보가 된다.
- 가설 2: round 21의 `-14.90%`는 sweep 순서와 SPOT run-to-run variance이며 standalone repeat에서는 10% 기준을 만족하지 않는다.
- 선택한 가설: 먼저 가설 2를 확인한다. 같은 항목이 round 20에서 이미 10% 미만으로 반복됐기 때문이다.

## 변경

- core 소스 변경: 없음
- perf 소스 변경: 없음
- 변경 이유: 수정 전 단독 반복으로 후보 적합성을 확인한다.
- perf 전용 변경이 아닌 이유: perf 코드는 수정하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: 측정 라운드이며 WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.
- 추가로 실행한 회귀 테스트: 소스 변경이 없으면 별도 test는 실행하지 않는다.

## 검증 예정

- build:
  - `cmake --build core/build -j$(nproc)`
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round22_spot_tcp_current_head_repeat`

## 결과

- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round22_spot_tcp_current_head_repeat`
  - 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_184023_round22_spot_tcp_current_head_repeat.txt`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - `META,commit`: `84e10b266`
  - load_avg: `1.76 2.37 3.51`
  - completion: success 1, fail 0, status complete
  - median throughput: `3,552,861.4 msg/s`
- 비교:
  - round 21 sweep 대비: `+7.15%`
  - 문제 report 대비: `-8.81%`
  - 과거 기준 대비: `-51.86%`

## 판정

- current HEAD standalone repeat에서도 `MULTI_SPOT tcp 64B`는 문제 report 대비 10% 이상 결손으로 재현되지 않았다.
- round 21의 `-14.90%`는 single-run sweep outlier로 보는 것이 맞다.
- 계획 기준상 이번 라운드에서 SPOT data-plane source 후보를 만들지 않는다.
- core 소스 변경 없음. perf 소스 변경 없음.
- 보안 하드닝 보호 항목은 건드리지 않았다.

## 다음 후보

- 현재 문제 report 대비 10% 이상 반복 결손은 없다.
- 과거 기준 대비 SPOT 장기 하락은 크지만, 현재 runner 조건에서는 반복 source 후보로 좁혀지지 않았다.
- 다음 라운드는 source 변경 대신 현재까지의 round 15-22 evidence를 정리해, perf 측정 의미 변화와 core 후보 부재를 분리한 뒤 남은 실험 후보를 하나만 고른다.
