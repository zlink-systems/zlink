# Round 37: SPOT publish ingress DONTWAIT admission

- goal: SPOT 64B one-way hot path에서 publish ingress admission 비용을 줄인다.
- 완료 기준: `MULTI_SPOT/tcp,tls/64` targeted 5-run에서 clean repeat 대비 10% 이상 반복 개선, 관련 core tests 통과, 효과 없으면 source 변경 원복.
- 시작 시각: 2026-06-15 01:58 KST
- 기준 commit: `72d893595`
- 시작 git status: core/perf source diff 없음. 기존 perf log 파일들은 untracked 상태로 둔다.
- 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 비교 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 최근 clean repeat: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_010731_round33_spot_tcp_tls_repeat.txt`
- 대상 pattern/transport/size: `MULTI_SPOT` / `tcp,tls` / `64B`

## 현재 수치

- corrected baseline:
  - `MULTI_SPOT/tcp/64 = 7379815.4`
  - `MULTI_SPOT/tls/64 = 6924687.4`
- problem report:
  - `MULTI_SPOT/tcp/64 = 3896078.6`
  - `MULTI_SPOT/tls/64 = 3739003.6`
- round33 clean repeat:
  - `MULTI_SPOT/tcp/64 = 3314983.0`
  - `MULTI_SPOT/tls/64 = 4123758.6`
- round36 rejected source candidate:
  - `MULTI_SPOT/tcp/64 = 3372086.2`
  - `MULTI_SPOT/tls/64 = 4225632.6`

## 가설

- 가설 1: SPOT public publish hot path는 `ZLINK_DONTWAIT`에서도 매 메시지마다 `std::function` 기반 `wait_for_queue_room()`을 거친다. nonblocking 경로는 대기하지 않으므로 queue room/closed 조건을 직접 확인하면 callable 포장과 간접 호출 비용을 줄일 수 있다.
- 가설 2: SPOT 64B 하락의 주원인은 publish ingress admission이 아니라 data-plane fanout 또는 perf 측정 의미 변화다. 이 경우 DONTWAIT admission 정리는 5% 미만 noise에 그친다.
- 선택한 가설: 가설 1을 먼저 최소 변경으로 검증한다. 이전 round에서 frame copy, encoded byte 계산, poller refresh, pump, message pool 후보가 모두 10% 개선을 만들지 못했기 때문이다.

## 읽은 코드

- `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`: `enqueue_publish_ingress()`가 queue lock 획득 뒤 DONTWAIT 여부와 무관하게 `wait_for_queue_room()`을 호출한다.
- `wait_for_queue_room()`은 `std::function<bool()>`을 인자로 받아 DONTWAIT일 때도 `ready_()`를 한 번 호출한다.
- perf SPOT publish 경로는 `zlink_spot_publish_part(..., ZLINK_PART_FINAL)` -> `spot_publish_no_sequence_check()` -> `enqueue_publish_ingress()`로 들어간다.

## 변경

- 변경 파일: `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`
- 변경 이유: DONTWAIT publish admission에서 대기 helper와 `std::function` 포장을 피한다.
- perf 전용 변경이 아닌 이유: public SPOT nonblocking publish의 실제 core runtime hot path를 줄이는 변경이다. perf runner/client/server 조건은 바꾸지 않는다.

## 후보 perf 1차

- build: `cmake --build core/build -j$(nproc)` 통과. WSL clock skew 경고가 있었으나 exit 0.
- focused test 1차:
  - command: `ctest --test-dir core/build --output-on-failure -R 'test_(spot_pubsub_scenario|spot_poller|spot_runtime_activation|spot_dispatch_event|spot_router_channel_peer|transport_matrix|multi_socket_contract_regressions|pubsub|pubsub_filter_xpub|xpub_nodrop)$|unittest_spot_data_plane_'`
  - result: `test_spot_router_channel_peer` 1회 실패.
- focused test 재확인:
  - `ctest --test-dir core/build --output-on-failure -R '^test_spot_router_channel_peer$' --repeat until-pass:2` 통과.
  - 같은 focused set 재실행 통과.
- targeted perf:
  - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp,tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round37_spot_dontwait_admission`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_020132_round37_spot_dontwait_admission.txt`
  - load average: `16.46 14.91 14.59`
  - result:
    - `MULTI_SPOT/tcp/64 = 3809480.0`
    - `MULTI_SPOT/tls/64 = 3418233.2`

## 1차 판정

- tcp는 round33 clean repeat `3314983.0` 대비 `+14.92%`로 신호가 있다.
- tls는 round33 clean repeat `4123758.6` 대비 `-17.10%`로 큰 악화다.
- transport별 결과가 혼재하므로 바로 유지하지 않는다. 변경을 원복한 뒤 같은 시간대 clean A/B를 재측정한다.

## 원복 후 clean A/B

- 변경 원복 후 build: `cmake --build core/build -j$(nproc)` 통과.
- command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp,tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round37_spot_clean_ab_after_revert`
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_020644_round37_spot_clean_ab_after_revert.txt`
- load average: `36.87 16.30 14.28`
- result:
  - `MULTI_SPOT/tcp/64 = 3884640.0`
  - `MULTI_SPOT/tls/64 = 3524177.4`

## 최종 판정

- 후보 tcp `3809480.0`보다 원복 clean tcp `3884640.0`이 더 높다.
- 후보 tls `3418233.2`보다 원복 clean tls `3524177.4`도 더 높다.
- 후보 1차의 tcp 상승은 source 변경 효과가 아니라 run-to-run variance로 판정한다.
- source 변경은 유지하지 않는다.
- `git diff -- core/src core/include core/tests bindings/c/perf` 결과 source diff 없음이어야 한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거: WS/WSS pending message, mtrie traversal, port parsing, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다. queue closed/backpressure/EAGAIN 의미는 기존과 같게 유지한다.
- 추가로 실행할 회귀 테스트: SPOT/PUBSUB focused tests.
