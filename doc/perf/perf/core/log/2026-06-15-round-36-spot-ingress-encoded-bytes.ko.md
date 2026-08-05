# Round 36: SPOT ingress encoded byte 계산 병합

- 목표: `MULTI_SPOT/tcp/64` 반복 gap에 대해 perf runner/client/server를 바꾸지 않고 core SPOT publish ingress hot path만 검증한다.
- 기준 baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 최근 clean repeat: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_010731_round33_spot_tcp_tls_repeat.txt`

## 선택한 후보

`enqueue_publish_ingress()`는 raw `zlink_msg_t` parts를 owned parts로 복사한 뒤
`spot_msg_parts_encoded_bytes()`로 복사된 frames를 다시 순회한다. 64B publish hot path에서는
이 순회가 매 메시지마다 반복된다.

이번 후보는 `copy_raw_parts_to_owned()`가 복사하면서 encoded byte 수를 함께 계산하도록 바꾼다.
invalid frame 방어와 caller part 소비 시점은 그대로 유지한다.

## 검증

- build: `cmake --build core/build -j$(nproc)` 통과.
- focused CTest 1차:
  - command: `ctest --test-dir core/build --output-on-failure -R 'test_(spot_pubsub_scenario|spot_poller|spot_runtime_activation|spot_dispatch_event|spot_router_channel_peer|transport_matrix|multi_socket_contract_regressions)$'`
  - result: `test_spot_pubsub_scenario`의 `test_spot_node_direct_local_and_child_interop` 1회 실패.
- focused CTest 재확인:
  - `ctest --test-dir core/build --output-on-failure -R '^test_spot_pubsub_scenario$' --repeat until-pass:2` 통과.
  - 같은 focused set 재실행 통과.

## Targeted perf

- command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp,tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round36_spot_ingress_encoded_bytes`
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_015100_round36_spot_ingress_encoded_bytes.txt`
- load average: `16.89 19.02 16.74`
- result:
  - `MULTI_SPOT/tcp/64 = 3372086.2`
  - `MULTI_SPOT/tls/64 = 4225632.6`

## 판정

- `tcp`는 round33 clean repeat `3314983.0` 대비 `+1.72%`라 10% 반복 개선 기준에 못 미친다.
- `tls`는 round33 clean repeat `4123758.6` 대비 `+2.47%`라 noise 범위다.
- source 변경은 유지하지 않는다.

## 원복

- `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp` 변경을 되돌렸다.
- `git diff -- core/src core/include core/tests bindings/c/perf` 결과 source diff 없음.
- restore build: `cmake --build core/build -j$(nproc)` 통과.
