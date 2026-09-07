# G-5 진행 (sonnet)

- 원인 확정: `bindings/c/perf/common/perf_zlink_part_helpers.hpp`의 `perf_measurement_part_count()`가
  매 호출마다 `std::getenv("PERF_PART_COUNT")`를 재조회. 호출처: send(perf_zlink_send_measurement_parts 등)
  1회/msg + recv(perf_single_one_way.hpp의 tail 체크, recv_router_router_header_flags 경로) 1회/msg = 2회/msg.
  multi(pubsub/reqrep), single 모두 이 헬퍼 공유.
- with_zmq에는 이 헬퍼/env 없음 → 대칭 변경 불필요.
- 수정: 함수 스코프 `static const size_t` 캐시로 변경(기존 `bench_debug_enabled()` 패턴과 동일). PERF_PART_COUNT는
  런치 스크립트에서만 설정되고 실행 중 안 바뀜(grep 확인).
- 완료: dev 빌드 OK, release --lib-only OK, perf single 7패턴×6트랜스포트(42/42), multi 7패턴 tcp 1024B(7/7) 성공.
  callgrind 축소셀(RR single, d12/d4 차분, PERF_LOCK): getenv calls/msg 1.996~2.03 → 0.00014, Ir/msg 15,806 → 14,324(−9.4%).
  보고서 core-rf-G-5-summary.md 작성 완료. 변경 파일: bindings/c/perf/common/perf_zlink_part_helpers.hpp만.
