# Round 21: current HEAD 64B sweep

- goal: 현재 HEAD에서 64B 전체 sweep를 실행해 실패 항목과 10% 이상 반복 gap 후보를 다시 선정한다.
- 완료 기준: current 64B sweep report를 만들고, 문제 report 및 과거 기준 대비 gap을 재계산한다. 실패가 있으면 다음 라운드 후보를 실패 안정화로 잡고, 10% 이상 gap이 있으면 해당 core hot path를 추적한다.
- 시작 시각: 2026-06-14 18:31:55 +0900
- 기준 commit: `1b661a99d`
- 시작 git status: core source diff 없음. round 9-20 로그 파일이 untracked 상태다.
- 과거 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 최근 현재 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_174702_round16_current_64b_lto.txt`
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_181947_round19_restored_oneway_refresh.txt`
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_182734_round20_spot_tcp_repeat.txt`
- 대상 pattern/transport/size: runner default pattern/transport / `64B`

## 가설

- 가설 1: 현재 HEAD sweep에서 실패 또는 문제 report 대비 10% 이상 gap이 다시 나타나 다음 core 후보를 특정할 수 있다.
- 가설 2: 현재 HEAD sweep도 문제 report 대비 10% 이상 gap이 없으며, 장기 하락은 perf 측정 의미 변화와 run-to-run variance가 섞여 있어 바로 source 후보가 되지 않는다.
- 선택한 가설: 먼저 가설 1을 검증한다. 전체 목표는 stream 단일 항목이 아니라 64B 전체 평균/중앙값 개선이므로, 다음 source 후보는 현재 sweep에서 다시 선정한다.

## 읽을 코드와 조건

- sweep에서 10% 이상 gap이 나온 pattern의 core hot path만 읽는다.
- 후보가 없으면 source를 수정하지 않는다.

## 변경

- core 소스 변경: 없음
- perf 소스 변경: 없음
- 변경 이유: 현재 HEAD 기준 실패/gap map을 갱신한다.
- perf 전용 변경이 아닌 이유: perf 코드는 수정하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: 측정 라운드이며 WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.
- 추가로 실행한 회귀 테스트: 소스 변경이 없으면 별도 test는 실행하지 않는다.

## 검증 예정

- build:
  - `cmake --build core/build -j$(nproc)`
- current 64B sweep:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round21_current_64b_sweep`

## 결과

- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
- current 64B sweep:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round21_current_64b_sweep`
  - 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_183228_round21_current_64b_sweep.txt`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - `META,commit`: `1b661a99d`
  - load_avg: `0.60 2.05 4.23`
  - completion: success 32, fail 0, status complete
- 문제 report 대비:
  - 공통 64B 26개 평균 `-2.22%`, 중앙값 `-2.73%`
  - one-way 64B 14개 평균 `-4.70%`, 중앙값 `-3.95%`
  - echo 64B 12개 평균 `+0.67%`, 중앙값 `+0.75%`
  - 10% 이상 하락 항목: `MULTI_SPOT tcp 64B` `-14.90%`
- 과거 기준 대비:
  - 전체 64B 32개 평균 `-15.41%`, 중앙값 `-13.64%`
  - one-way 64B 16개 평균 `-31.72%`, 중앙값 `-26.70%`
  - echo 64B 16개 평균 `+0.90%`, 중앙값 `-6.72%`

## 판정

- failure 0은 유지됐다.
- 문제 report 대비 10% 이상 gap은 `MULTI_SPOT tcp 64B` 하나뿐이다.
- 그러나 round 20의 standalone 5-run repeat에서는 같은 항목이 문제 report 대비 `-5.37%`였고, round 21은 single-run sweep다.
- 따라서 `MULTI_SPOT tcp 64B`는 바로 core source 후보로 보지 않고, 현재 HEAD에서 standalone repeat로 먼저 재확인한다.
- core 소스 변경 없음. perf 소스 변경 없음.
- 보안 하드닝 보호 항목은 건드리지 않았다.

## 다음 후보

- `MULTI_SPOT tcp 64B` current HEAD standalone repeat.
- 반복 10% 이상 결손이면 SPOT data-plane publish ingress/local fanout 경로를 추적한다.
- 반복되지 않으면 현재 evidence에서 문제 report 대비 10% 이상 안정 gap은 없다고 판정한다.
