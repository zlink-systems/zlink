# Round 111: STREAM packet callback unlocked

## 이번 라운드 목표

- STREAM packet dispatch에서 pipe packet-state lock을 사용자 packet handler 호출 동안 잡고 있는 비용과 재진입 위험을 줄인다.
- 완료 기준:
  - focused STREAM tests 통과.
  - `STREAM` 64B tcp/tls/wss focused perf에서 실패 0개.
  - 하락 항목이 있거나 5% 미만 효과면 source를 되돌린다.

## 기준 report

- round98 STREAM-only:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_154759_round98_stream_transport_transition_failure_recheck.txt`
- round100 reduced:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_155904_round100_reduced_stream_transition_stability.txt`
- round110 low-load current:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_171900_round110_lowload_current_guard.txt`

## 병목 가설

1. `stream_dispatch_packet_msg_from_io()`는 packet-state lock을 잡은 채 사용자 packet handler를 호출한다. 64B echo에서는 callback이 바로 응답 send를 수행하므로 내부 state lock을 불필요하게 길게 잡을 수 있다.
2. 완성된 packet은 `state.header`/`state.body`에서 지역 `msg_t`로 move하고 `state.reset()`한 뒤에는 packet-state lock이 필요 없다.
3. lock을 callback 전에 풀면 packet state 정보 은닉은 유지하면서 lock hold와 callback 재진입 위험을 줄일 수 있다.

## POSD 검토

- public API, wire format, parser state 구조는 바꾸지 않는다.
- 내부 lock의 책임을 packet-state 보호로 좁힌다. 사용자 callback 실행까지 lock 책임을 넓히지 않는다.
- complete-frame parser fast path처럼 별도 parser를 추가하지 않는다.

## 재검토 결과

- source 적용 전 안전성 재검토에서 미채택으로 결정했다.
- `pipe_t::reset_stream_packet_state()`는 같은 `stream_packet_dispatch_sync()` 락으로
  packet state를 보호한다. 콜백 호출 전에 이 락을 풀면 콜백 중
  `zlink_stream_detach()` 또는 dispatch stop 계열 호출이 packet state를 reset할 수 있고,
  콜백 뒤에 같은 dispatch 루프가 다시 락을 잡고 남은 payload 처리를 이어 간다.
- 이 구조는 packet 조립 상태 보호와 callback lifecycle 의미를 분리해서 호출자가 관찰할 수
  있는 동시성 의미를 바꿀 수 있다. 성능 hot path 최적화가 내부 계약을 더 흐리게 만들기
  때문에 POSD의 정보 은닉과 복잡성을 아래로 내리는 원칙에 맞지 않는다.
- 따라서 build/test/perf 실행 전 source를 되돌렸다. 이 라운드는 수치 비교 대상이 아니다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - STREAM packet dispatch lock scope만 변경한다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거를 되돌리지 않는다.
  - maxmsgsize 초과 시 disconnect/terminate 처리는 기존 위치와 의미를 유지한다.
  - mtrie, port parsing, IPC unlink, decoder/message/send guard 정책을 수정하지 않는다.
  - perf runner/client/server를 수정하지 않는다.

## 실행 예정

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure -R 'test_multi_stream_server_reassembly|test_stream_(socket|threadsafe|send_blocking_wakeup|fastpath|routing_id_size)'
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round111_stream_packet_callback_unlocked
```

## 최종 판단

- 미채택.
- 최종 source diff: 없음.
