# Round 106: ASIO handler allocator WSS/REQREP probe

- goal: WSS/ASIO callback hot path에서 handler allocation 비용을 줄여 SPOT_REQREP/wss와 STREAM/wss 회귀를 완화한다.
- 시작 시각: 2026-06-15 KST
- 기준 문서: `doc/plan/perf/core/core-library-performance-improvement-plan.ko.md`
- 비교 기준:
  - May26 full: `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
  - round100: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_155904_round100_reduced_stream_transition_stability.txt`
  - round105: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_163053_round105_spot_retained_fastpath_recheck.txt`
- 현재 source diff:
  - retained `zlink_spot_send_spot_part()` FINAL-only fast path
  - 이번 후보: ASIO read/write/handshake handler allocator 적용 범위를 STREAM 전용에서 전체 ASIO engine으로 넓힌다.

## 병목 가설

1. WSS는 TCP/TLS보다 async callback 비용이 크고, SPOT_REQREP/wss와 STREAM/wss가 모두 낮다.
   - WebSocket frame write/read 자체를 우회하면 보안/프로토콜 위험이 크다.
   - 대신 ASIO handler allocation을 줄이면 transport 의미를 바꾸지 않고 callback churn 비용만 줄일 수 있다.
2. 현재 `handler_allocator`는 STREAM에만 적용된다.
   - ASIO engine은 read/write/handshake 각각 하나의 pending operation을 유지하므로, 같은 allocator를 비-STREAM socket에도 적용할 수 있다.
   - 새 상태를 추가하지 않고 기존 allocator 슬롯을 재사용한다.
3. 이미 폐기한 후보는 반복하지 않는다.
   - WSS gather-write capability enable은 round76에서 효과가 없어 폐기했다.
   - TLS `async_write_some()` 변경은 round80에서 하락으로 폐기했다.
   - WebSocket `write_some()` speculative enable은 complete frame synchronous write라 blocking 위험이 있어 제외한다.

## 먼저 검증할 가설

- H1: 전체 ASIO handler allocator 적용이 WSS focused set에서 하락 없이 개선되는가.
- H2: WSS가 좋아져도 tcp/tls 또는 SPOT_SENDSEND retained 효과를 해치지 않는가.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거를 되돌리지 않는다.
  - WebSocket frame read/write 의미와 max message 정책을 바꾸지 않는다.
  - mtrie, port parsing, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 수정하지 않는다.
- 추가로 실행할 회귀 테스트:
  - ASIO transport/stream/spot request-reply 관련 CTest
  - WSS focused perf

## 실행

```bash
ctest --test-dir core/build -R 'transport_matrix|stream|pubsub|xpub|spot|zmp_request_reply|multi_socket_contract' --output-on-failure
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB,SPOT_SENDSEND,SPOT_REQREP,STREAM --transports wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round106_asio_handler_alloc_wss_focus
```

- build: pass
- CTest: 65/65 pass
- perf status: complete, fail 0
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`

## 결과

| pattern | transport | May26 full | round100 | round105 | round106 | 판단 |
|---------|-----------|------------|----------|----------|----------|------|
| MULTI_PUBSUB | wss | 2760571.0 | 2465419.0 | n/a | 2506484.6 | round100보다 +1.67%지만 May26보다 낮음 |
| MULTI_SPOT_SENDSEND | wss | 252557.8 | 244308.4 | 252776.8 | 244671.0 | round105보다 -3.21% |
| MULTI_SPOT_REQREP | wss | 219301.2 | 214421.2 | 210854.2 | 204028.6 | round105보다 -3.24%, round100보다 -4.85% |
| MULTI_STREAM | wss | 184722.2 | 175863.2 | n/a | 177352.2 | round100보다 +0.85%지만 May26보다 낮음 |

## 판단

- 후보 폐기. PUBSUB/wss와 STREAM/wss의 작은 상승은 SPOT_SENDSEND/wss, SPOT_REQREP/wss 하락을 상쇄하지 못한다.
- 특히 retained SPOT fast path의 wss 효과를 되깎고, SPOT_REQREP/wss도 낮아져 "하락 항목 없이 작은 개선을 채택"한다는 기준을 충족하지 못한다.
- POSD 관점에서도 전체 ASIO engine에 allocator 정책을 넓히려면 성능 근거가 명확해야 한다. 이번 결과는 정책 범위 확장을 정당화하지 못한다.
- source는 되돌렸다. round106 이후 유지되는 source 변경은 `zlink_spot_send_spot_part()` FINAL-only fast path뿐이다.
- 보안 하드닝 보존:
  - WS/WSS pending message 전체 사본 제거를 되돌리지 않았다.
  - WebSocket frame read/write 의미와 max message 정책을 바꾸지 않았다.
  - mtrie, port parsing, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 수정하지 않았다.

## 원복 확인

```bash
cmake --build core/build -j$(nproc)
git diff -- core/src/runtime/engine/asio/asio_engine.cpp core/src/runtime/engine/asio/asio_engine.hpp
git diff --check
```

- rebuild after revert: pass
- ASIO source diff: 없음
- `git diff --check`: pass
