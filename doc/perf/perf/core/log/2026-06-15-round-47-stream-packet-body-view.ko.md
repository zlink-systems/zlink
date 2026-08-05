# Round 47: stream packet body view

- 목표: `MULTI_STREAM/tcp/64B`를 historical baseline
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`의
  `400,124.6 ops/s` 기준으로 다시 평가하고, packet callback hot path에서 core-only
  비용을 줄인다.
- 기준 확인:
  - baseline `MULTI_STREAM/tcp/64`: `400,124.6 ops/s`
  - clean 재현: `332,541.6 ops/s`
  - gap: `-16.9%`
- 후보:
  - `stream_dispatch_packet_msg_from_io()`에서 header가 없고 body가 현재 raw message
    안에 완전히 들어 있는 packet은 `state.body.init_size()+memcpy` 대신
    `msg_t::init_view()`로 body part를 만든다.
  - callback이 받은 `zlink_msg_t`를 닫거나 이동할 수 있는 소유권 계약은 유지한다.
    view message가 원본 message storage의 refcount를 잡고, callback 이후 core가
    원본 raw message를 reset해도 body part는 독립적으로 close 가능하다.
- 판정 기준:
  - stream 관련 ctest 통과.
  - `MULTI_STREAM/tcp/64B`가 clean 재현 대비 5% 이상 개선되어야 유지한다.
  - 5% 미만이거나 실패하면 원복한다.
- 결과:
  - build: `cmake --build core/build -j$(nproc)` 통과.
  - ctest: `test_stream|test_multi_stream_server_reassembly|test_transport_matrix`
    21/21 통과.
  - perf:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_035112_round47_body_view_stream_tcp64.txt`
  - `MULTI_STREAM/tcp/64`: `322,603.2 ops/s`
  - clean 재현 `332,541.6 ops/s` 대비 `-3.0%`.
- 판정:
  - 5% 개선 신호가 없고 오히려 낮아져 원복한다.
