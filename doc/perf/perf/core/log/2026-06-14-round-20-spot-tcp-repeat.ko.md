# Round 20: SPOT tcp 64B 단독 반복 확인

- goal: `MULTI_SPOT tcp 64B`가 문제 report 대비 10% 이상 반복 결손인지 확인한다.
- 완료 기준: restored-source `core/build`로 `MULTI_SPOT tcp 64B`를 단독 반복 측정하고, 10% 이상 결손이 재현되면 SPOT data-plane publish ingress/local fanout 경로를 추적한다. 재현되지 않으면 core 변경을 보류한다.
- 시작 시각: 2026-06-14 18:26:50 +0900
- 기준 commit: `0678c4baa`
- 시작 git status: core source diff 없음. round 9-19 로그 파일이 untracked 상태다.
- 과거 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 반복 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_174702_round16_current_64b_lto.txt`
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_181947_round19_restored_oneway_refresh.txt`
- 대상 pattern/transport/size: `MULTI_SPOT` / `tcp` / `64B`

## 현재 수치 요약

- 문제 report `MULTI_SPOT tcp 64B`: `3,896,078.6 msg/s`
- round 16 current `MULTI_SPOT tcp 64B`: `3,770,070.0 msg/s`, 문제 report 대비 약 `-3.23%`
- round 19 restored-source refresh `MULTI_SPOT tcp 64B`: `3,505,668.5 msg/s`, 문제 report 대비 약 `-10.02%`
- 과거 기준 `MULTI_SPOT tcp 64B`: `7,379,815.4 msg/s`

## 가설

- 가설 1: `MULTI_SPOT tcp 64B`는 단독 반복에서도 문제 report 대비 10% 이상 낮게 재현되며, SPOT data-plane publish ingress/local fanout 경로가 core 수정 후보가 된다.
- 가설 2: round 19의 `-10.02%`는 SPOT tcp의 run-to-run variance이며, 단독 반복에서는 round 16처럼 10% 기준을 만족하지 않는다.
- 선택한 가설: 먼저 가설 2를 확인한다. 계획은 10% 이상 반복되는 차이만 의미 있는 회귀/개선으로 보기 때문이다.

## 읽을 코드와 조건

- 10% 이상 반복 결손이면 아래 경로를 읽는다.
  - `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`
  - `core/src/runtime/services/spot/data_plane/spot_data_plane_pending.cpp`
  - `core/src/runtime/services/spot/node/spot_node_pubsub_fanout.cpp`
  - `core/src/runtime/services/spot/common/spot_message_parts_internal.hpp`
- 10% 미만이면 source 후보를 만들지 않는다.

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
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round20_spot_tcp_repeat`

## 결과

- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round20_spot_tcp_repeat`
  - 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_182734_round20_spot_tcp_repeat.txt`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - `META,commit`: `0678c4baa`
  - load_avg: `0.56 3.30 5.28`
  - completion: success 1, fail 0, status complete
  - median throughput: `3,686,940.0 msg/s`
- 비교:
  - round 16 current 대비: `-2.20%`
  - round 19 restored-source refresh 대비: `+5.17%`
  - 문제 report 대비: `-5.37%`
  - 과거 기준 대비: `-50.04%`

## 판정

- `MULTI_SPOT tcp 64B`의 round 19 `-10.02%` 하락은 단독 5-run 반복에서 재현되지 않았다.
- 계획 기준상 문제 report 대비 10% 이상 반복 결손이 아니므로, 이번 라운드에서 SPOT data-plane source 후보를 만들지 않는다.
- core 소스 변경 없음. perf 소스 변경 없음.
- 보안 하드닝 보호 항목은 건드리지 않았다.

## 다음 후보

- 문제 report 대비 반복 10% 결손은 현재 targeted evidence에서 확인되지 않는다.
- 과거 기준 대비 SPOT 장기 하락은 여전히 크지만, round 15/17/19/20의 측정 의미 변화와 run-to-run variance 때문에 core hot path 회귀로 확정되지 않았다.
- 계속 진행한다면 측정 의미를 바꾸지 않는 범위에서 SPOT data-plane 내부의 구조 개선 후보를 read-only로 더 좁혀야 한다. 성능 후보가 되려면 source 변경 전후 같은 runner 조건에서 10% 이상 반복 개선을 보여야 한다.
