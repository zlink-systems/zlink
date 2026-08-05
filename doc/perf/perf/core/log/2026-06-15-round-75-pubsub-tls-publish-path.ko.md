# Round 75 - PUBSUB/tls publish path candidate

## 목적

- May26 full 기준에서 반복 하락한 `MULTI_PUBSUB/tls/64B`를 좁혀 본다.
- SPOT 복구로 되돌린 part-helper 변경이 일반 PUB 소켓의 `zlink_publish_part` 경로에 영향을 주는지 확인한다.
- POSD 기준상 새 상태나 복잡한 캐시를 추가하지 않는 후보만 테스트한다.

## 확인

- 일반 PUB/XPUB socket handle은 socket-owned part-helper state를 사용한다.
- SPOT handle, SPOT pub/sub side handle, SPOT node handle만 global part-helper map 경로를 탄다.
- 따라서 현재 유지 중인 SPOT service-owned part-helper 복구 제거는 일반 `MULTI_PUBSUB`의 PUB socket publish 경로 원인으로 보기 어렵다.

## 후보

- `zlink_publish_part(..., ZLINK_PART_FINAL)`에서 열린 multipart sequence가 없으면 `submit_simple_part` 상태 머신 대신 기존 `logical_multipart_publish(..., part_count=1)`로 위임한다.
- 새 상태를 추가하지 않고 기존 publish 계약 구현을 재사용하므로 설계 비용은 낮다.

## 검증

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|public_inproc_multipart_send|transport_matrix|multi_socket_contract_regressions|backpressure_oneway_matrix|backpressure_matrix)$|unittest_service_mode_policy'
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tls --duration 5 --runs 7 --connect-ready-timeout-ms 5000 --results-tag round75_publish_part_final_fastpath_pubsub_tls_candidate
```

- build: 통과
- ctest: 7/7 통과
- perf report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_132614_round75_publish_part_final_fastpath_pubsub_tls_candidate.txt`
- runtime: `core/build/lib/libzlink.so.6.0.4`
- load_avg: `29.64 11.69 7.94`
- result: `MULTI_PUBSUB/tls/64B = 2,259,968.2 ops/s`

## 판정

- round71 current low-load recheck `2,265,688.2 ops/s` 대비 약 `-0.25%`.
- May26 full 기준 하락도 해소하지 못했다.
- 후보는 되돌렸다.

## 다음

- `PUBSUB/tls` 하락은 part-helper ownership 복구가 아니라 XPUB fanout 또는 TLS transport/write path 쪽으로 계속 좁힌다.
