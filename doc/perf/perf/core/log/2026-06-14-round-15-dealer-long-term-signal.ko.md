# Round 15: DEALER 장기 하락 신호 분리

- goal: 과거 기준 대비 `MULTI_DEALER_DEALER` 64B tcp 하락이 core runtime 회귀인지 확인한다.
- 시작 시각: 2026-06-14 17:29:02 +0900
- 기준 commit: `5e3c438a2`
- 시작 git status: round 9-14 로그만 새 파일로 있음. core 소스 변경 없음.
- 과거 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 현재 반복 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_171942_round13_dealer_stream_64_repeat_current.txt`
- 대상 pattern/transport/size: `MULTI_DEALER_DEALER` / `tcp` / `64B`

## 가설

- 가설 1: `DEALER` 성공 hot path 자체가 느려졌다.
- 가설 2: 과거 기준과 현재 측정의 build 또는 perf runner 조건이 달라 장기 하락처럼 보인다.
- 선택한 가설: 먼저 가설 2를 확인한다. round 13에서 현재 zero-fail 기준 대비 `DEALER` 추가 하락은 없었고, baseline 대비 약 -25%만 남았기 때문이다.

## 읽은 코드와 조건

- `core/src/runtime/sockets/common/socket_base_msg.cpp`: baseline과 현재의 `send_direct_with_retry()` 성공 경로는 submit-retry 실패 분기 외에는 거의 같다.
- `core/src/runtime/sockets/internal/lb.cpp`: baseline과 현재 모두 one active pipe fast path를 가진다.
- `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp`: non-STREAM `DEALER`에 적용되는 batch 정책은 baseline과 현재가 실질적으로 같다.
- `bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp`: 현재 perf client는 active window 뒤 stop token을 보낸다.
- `bindings/c/perf/multi/src/perf_multi_dealer_dealer_server.cpp`: 현재 perf server는 stop token 처리를 포함하고, receive loop에서 `perf_socket_poll(..., -1)` 경로를 쓴다. baseline은 timeout 기반 receive window였다.

## 변경

- core 소스 변경: 없음
- perf 소스 변경: 없음
- build 산출물 변경: 현재 `core/build`가 `ENABLE_LTO=OFF` 캐시로 남아 있어, repo 기본값과 baseline 조건에 맞춰 `cmake -S core -B core/build -DENABLE_LTO=ON` 후 `cmake --build core/build -j$(nproc)`를 실행했다.
- perf 전용 변경이 아닌 이유: 소스 변경이 아니라 실제 `core/build` runtime을 repo 기본 Release 구성으로 맞춘 것이다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않았다.
- 추가로 실행한 회귀 테스트: `cmake --build core/build -j$(nproc)`가 test binary까지 빌드 완료했다. 소스 변경은 없어 별도 focused ctest는 실행하지 않았다.

## 검증

- baseline worktree:
  - 생성: `git worktree add --detach /tmp/zlink-perf-cb605 cb605c6c1`
  - build: `cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DWITH_DOCS=OFF -DWITH_TLS=ON -DBUILD_BENCHMARKS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && cmake --build core/build -j$(nproc)`
  - runtime: `/tmp/zlink-perf-cb605/core/build/lib/libzlink.so.6.0.0`
  - baseline build cache: `ENABLE_LTO=ON`
- baseline tcp-only rerun:
  - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER --transports tcp --duration 5 --runs 2 --results-tag round15_cb605_dealer_tcp_repeat`
  - result: `/tmp/zlink-perf-cb605/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_173704_round15_cb605_dealer_tcp_repeat.txt`
  - load_avg: `0.67 3.76 3.10`
  - completion: success 1, fail 0
  - median throughput: `3,980,502.1 msg/s`
- current tcp-only before LTO cache correction:
  - result: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_173737_round15_current_dealer_tcp_repeat.txt`
  - load_avg: `1.11 3.53 3.05`
  - median throughput: `2,945,305.6 msg/s`
- current tcp-only with baseline `connect_ready_timeout_ms=5000`:
  - result: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_173806_round15_current_dealer_tcp_connect5000.txt`
  - load_avg: `1.02 3.29 2.98`
  - median throughput: `2,944,434.9 msg/s`
- current tcp-only after `ENABLE_LTO=ON` rebuild:
  - result: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_174337_round15_current_dealer_tcp_lto_on.txt`
  - load_avg: `1.01 6.27 5.10`
  - median throughput: `2,944,896.1 msg/s`

## 판정

- baseline commit의 `DEALER` tcp 64B 고점은 tcp-only 조건에서 재현됐다.
- current의 `connect_ready_timeout_ms`를 baseline의 `5000`으로 맞춰도 수치는 변하지 않았다.
- current `core/build`를 repo 기본값인 `ENABLE_LTO=ON`으로 다시 빌드해도 수치는 변하지 않았다.
- baseline과 current의 core `DEALER`/`lb`/non-STREAM ASIO 정책은 장기 -26%를 바로 설명할 만큼 다르지 않다.
- 반면 perf client/server는 baseline 이후 의미 있는 변화가 있다. 현재 `DEALER_DEALER` perf는 stop token과 무기한 poll 중심으로 바뀌어 baseline의 timeout 기반 receive window와 같은 측정이라고 단정할 수 없다.
- 따라서 이 라운드에서는 장기 `DEALER` -26%를 core source 회귀로 확정하지 않는다. perf runner 의미 변경 검증이 먼저 필요하며, 계획 범위상 perf 수정은 core 성능 개선으로 계산하지 않는다.

## 다음 후보

- core-only 최적화 후보는 현재 반복 측정상 10% 이상 안정 결손이 남은 항목을 다시 선정해야 한다.
- 장기 기준 복구를 계속 보려면, current perf source에 baseline receive-window 의미를 임시로 복원한 read-only 비교 또는 perf 변경 이력 별도 감사가 필요하다.
