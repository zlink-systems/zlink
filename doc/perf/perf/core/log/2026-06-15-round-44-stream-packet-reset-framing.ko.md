# Round 44: STREAM packet state framing reset 후보

- 목표: `MULTI_STREAM/tcp/64B` historical baseline `400,124.6 ops/s` 회복.
- 시작 기준:
  - round43 clean sweep `MULTI_STREAM/tcp/64B`: `320,996.6 ops/s`
  - round41 clean focused recheck: `325,532.2 ops/s`
- 후보:
  - stream packet dispatch에서 완성된 packet의 `state.header/body`를 callback용
    `msg_t`로 move한 직후 `state.reset()` 대신 framing 필드만 초기화했다.
  - 근거: `msg_t::move()`는 source를 빈 initialized message로 되돌리므로, 완료 path에서
    바로 close/init를 반복하는 비용은 중복일 수 있다.
- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- test:
  - `ctest --test-dir core/build -R 'test_stream_socket|test_socket_with_handler|test_multi_stream_server_reassembly|test_stream_fastpath|test_stream_send_blocking_wakeup|test_stream_threadsafe|test_thread_safe_contract_policy|unittest_msg_view' --output-on-failure`
  - 통과, 22/22.
- perf:
  - 명령:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round44_stream_packet_reset_framing_candidate`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_033213_round44_stream_packet_reset_framing_candidate.txt`
  - runtime:
    `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - load_avg: `18.98 10.21 5.91`
  - 결과: `333,518.8 ops/s`
- 판정:
  - round43 clean `320,996.6` 대비 `+3.9%`
  - round41 clean focused `325,532.2` 대비 `+2.5%`
  - 5% 미만이라 노이즈 구간이며, 목표 `400kops`와 여전히 멀다.
  - 후보는 유지하지 않고 원복한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 변경 파일은 stream packet state reset 내부이며 callback lifetime, decompression, handle
  cleanup 항목을 건드리지 않았다.
- 후보는 원복하므로 최종 보안 의미 변경 없음.
