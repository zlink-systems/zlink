# Round 18: STREAM tcp 64B 후보 반복 확인

- goal: round 16에서 문제 report 대비 가장 낮게 남은 `MULTI_STREAM tcp 64B`가 10% 이상 안정 결손인지 확인한다.
- 완료 기준: LTO ON `core/build`로 STREAM tcp 64B를 단독 반복 측정하고, 10% 이상 반복 결손이면 core STREAM routing-id/send 후보를 추적한다. 10% 미만이면 core 변경을 보류한다.
- 시작 시각: 2026-06-14 18:06:34 +0900
- 기준 commit: `2e327a74a`
- 시작 git status: `bindings/python/samples/*`, `bindings/python/tests/test_sample_alignment.py` 변경이 있음. perf/core 작업과 무관하므로 건드리지 않는다. round 9-17 로그 파일이 untracked 상태다.
- 과거 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 후보 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_174702_round16_current_64b_lto.txt`
- 대상 pattern/transport/size: `MULTI_STREAM` / `tcp` / `64B`

## 가설

- 가설 1: STREAM tcp 64B의 `-7.11%` 하락은 단독 반복에서 10% 이상으로 재현되어 core STREAM send/read batching 후보가 된다.
- 가설 2: full targeted set 안에서 나온 `-7.11%`는 측정 순서나 STREAM 10000-client 노이즈이며, 단독 반복에서는 10% 기준을 만족하지 않는다.
- 선택한 가설: 먼저 가설 2를 확인한다. 계획은 5% 미만을 오차, 10% 이상 반복 차이만 의미 있는 회귀/개선으로 보기 때문이다.

## 읽을 코드와 조건

- `core/build/CMakeCache.txt`: `ENABLE_LTO=ON`, `CMAKE_BUILD_TYPE=Release`, `WITH_TLS=ON`.
- `core/src/runtime/sockets/stream/*`: 10% 이상 반복 결손일 때만 추적한다.
- `core/src/runtime/engine/asio/*`: 10% 이상 반복 결손일 때만 추적한다.

## 변경

- core 소스 변경: 없음
- perf 소스 변경: 없음
- 변경 이유: 수정 전 단독 반복으로 후보 적합성을 확인한다.
- perf 전용 변경이 아닌 이유: perf 코드는 수정하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: 측정 라운드이며 WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.
- 추가로 실행한 회귀 테스트: 측정 전 `cmake --build core/build -j$(nproc)`를 실행한다.

## 검증 예정

- build:
  - `cmake --build core/build -j$(nproc)`
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round18_stream_tcp_repeat`

## 결과

- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round18_stream_tcp_repeat`
  - 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_180820_round18_stream_tcp_repeat.txt`
  - completion: success 1, fail 0, status complete
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 비교:
  - 문제 report `perf_c_multi_linux_20260614_103936.txt`: `MULTI_STREAM tcp 64B` throughput `299,395.0 msg/s`
  - round 16 full targeted set: `278,119.8 msg/s`로 문제 report 대비 약 `-7.11%`
  - round 18 단독 반복: `304,577.2 msg/s`로 문제 report 대비 약 `+1.73%`

## 판정

- `MULTI_STREAM tcp 64B`의 round 16 `-7.11%` 하락은 단독 반복에서 재현되지 않았다.
- 계획 기준상 10% 이상 반복 결손이 아니며, 이번 라운드에서 core STREAM send/read 후보를 추적하지 않는다.
- core 소스 변경 없음. perf 소스 변경 없음.
