# Round 9: publish part single-frame fast path

## 범위

- 대상: `core/src/api/socket/socket_message_send_api.cpp`
- 목표: `zlink_publish_part()` 의 단일 `FINAL` publish가 generic part-helper
  상태 기계를 거치지 않고 기존 publish parts 경로와 같은 의미로 전송되는지
  검증한다.
- 제외: perf runner, perf client/server, benchmark option 변경.

## 기준

- 시작 git status: 깨끗함.
- 기준 HEAD: `f3659d25c`
- 직전 zero-fail 전체 기준: `perf_c_multi_linux_20260614_151925.txt`
- Round 8 targeted PUBSUB 기준: `perf_c_multi_linux_20260614_163551.txt`

## 가설

1. `bindings/c/perf` 의 `perf_zlink_publish_parts()` 는 `zlink_publish_part()` 를
   part마다 호출한다. part 수가 1이고 `ZLINK_PART_FINAL` 인 PUBSUB hot path에서도
   현재 구현은 `submit_simple_part()` 로 들어가 handle state 조회, shared pointer,
   send scope 준비, spec 문자열 복사를 매번 수행한다.
2. SPOT publish part는 이미 `ZLINK_PART_FINAL` 이고 send sequence가 없으면
   `spot_publish_no_sequence_check()` 로 우회한다. PUB/XPUB도 같은 조건에서
   `logical_multipart_publish()` 로 직접 보내면 공개 API 의미를 유지하면서
   generic part-helper 비용을 줄일 수 있다.

## 읽은 코드

- `bindings/c/perf/common/perf_zlink_part_helpers.hpp`: PUBSUB perf wrapper는
  `zlink_publish_part()` 를 호출한다.
- `core/src/api/socket/socket_message_send_api.cpp`: `zlink_send_part()` 와
  `zlink_send_part_rid()` 는 단일 `FINAL` fast path가 있지만
  `zlink_publish_part()` 에는 같은 fast path가 없다.
- `core/src/api/spot/core/service_spot_api.cpp`: `zlink_spot_publish_part()` 는
  단일 `FINAL` 에서 sequence가 없으면 이미 직접 publish 경로를 사용한다.

## 변경 계획

- `zlink_publish_part()` 에서 아래 조건을 모두 만족할 때만 fast path를 탄다.
  - `part_flag_ == ZLINK_PART_FINAL`
  - handle에 active send sequence가 없음
  - socket type이 PUB 또는 XPUB
- fast path는 `publish_socket_parts()` 를 직접 호출하고, 기존 part-helper 경로와
  같이 반환 전 `consume_send_part()` 를 호출한다.

## 실험 결과

- 변경 파일: `core/src/api/socket/socket_message_send_api.cpp`
- 변경 내용: `zlink_publish_part()` 의 단일 `FINAL` publish에서 active send
  sequence가 없으면 `publish_socket_parts()` 를 직접 호출하는 fast path를 추가했다.
- Build: `cmake --build core/build -j$(nproc)` 통과.
- Focused test:
  `ctest --test-dir core/build --output-on-failure -R 'test_public_inproc_multipart_send|test_pubsub$|test_pubsub_filter_xpub|test_xpub_nodrop|test_transport_matrix|test_spot_pubsub_scenario|test_backpressure_(oneway_)?matrix_spot_regression|unittest_spot_data_plane_'`
  - 18/18 통과.
- Targeted PUBSUB:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern MULTI_PUBSUB --transports tcp,tls,ws,wss --msg-sizes 64 --duration 5`
  - Report: `perf_c_multi_linux_20260614_164255.txt`
  - success 4, fail 0
  - 기준 `perf_c_multi_linux_20260614_151925.txt` 대비 throughput:
    tcp +4.91%, tls -0.82%, ws -2.52%, wss -4.14%
  - Round 8 targeted `perf_c_multi_linux_20260614_163551.txt` 대비:
    tcp +0.39%, tls -1.27%, ws -1.46%, wss -4.11%

## 판단

- tcp만 5% 미만 상승했고 나머지 transport는 하락했다.
- part-helper 우회는 PUBSUB 64B 회귀를 의미 있게 줄이지 못했다.
- source 변경은 제거했다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: decoder/message/send guard
- 보안 의미를 유지한 근거: part pointer, send flags, socket handle, socket type,
  active sequence 검사를 유지한다. 실패와 성공 모두 part ownership 정리를 유지한다.
- 추가로 실행한 회귀 테스트: PUBSUB/XPUB focused tests, transport matrix,
  targeted PUBSUB perf.
