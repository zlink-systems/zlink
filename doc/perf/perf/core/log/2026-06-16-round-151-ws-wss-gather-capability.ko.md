# Round 151: WS/WSS gather capability

## 목표

- `SPOT/wss`와 WS/WSS 계열 64B 하락을 transport write path에서 좁힌다.
- 이미 구현된 WS/WSS `async_writev()`를 engine gather path가 사용할 수 있는지 확인한다.
- tcp/tls 경로와 perf runner/client/server는 수정하지 않는다.

## 기준

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round149 refresh:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_052834_round149_pubsub_spot_worst_refresh.txt`

## 시작 상태

- retained source diff:
  - `zlink_spot_send_spot_part()` 단일 `ZLINK_PART_FINAL` fast path
- 새 후보 적용 전에는 WS/WSS transport source diff 없음.

## 병목 가설

1. `ws_transport_t`와 `wss_transport_t`는 `async_writev()`를 구현하지만 `supports_gather_write()`가 false라
   `asio_engine_t::prepare_gather_output()`이 이 경로를 쓰지 못한다.
2. WS/WSS의 `async_writev()`는 header와 body를 Beast `async_write(buffers)`에 넘겨 하나의 WebSocket frame으로
   쓰므로 WebSocket frame 의미를 유지한다.
3. 이 후보는 tcp/tls에는 닿지 않고 WS/WSS transport capability만 바로잡는다. 따라서 round123의 일반 gather
   threshold probe나 round136 TLS gather 후보와 다른 축이다.

## 먼저 검증할 가설

- WS/WSS `supports_gather_write()`를 true로 바꾸면 `PUBSUB`/`SPOT` ws,wss 64B가 하락 없이 개선되는지 본다.

## POSD 검토

- 새 public API를 추가하지 않는다.
- frame 조립 정책은 transport 내부에 숨긴다.
- 이미 구현된 transport capability를 engine에 정확히 알리는 변경이다.
- WS/WSS pending message 전체 사본 제거를 되돌리지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 후보 적용 전 없음.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거를 되살리지 않는다.
  - mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서, decoder/message/send guard, `maxmsgsize` 정책을
    수정하지 않는다.

## 적용한 후보

- `core/src/runtime/transports/ws/ws_transport.hpp`
  - `supports_gather_write()`를 `false`에서 `true`로 바꿨다.
- `core/src/runtime/transports/tls/wss_transport.hpp`
  - `supports_gather_write()`를 `false`에서 `true`로 바꿨다.

후보는 WS/WSS transport capability만 바꾸며 public API와 perf code는 수정하지 않았다.

## 기능 검증

```bash
cmake --build core/build --target libzlink -j$(nproc)
ctest --test-dir core/build --output-on-failure -R 'test_(transport_matrix|pubsub|pubsub_filter_xpub|xpub_nodrop|spot_pubsub_scenario|spot_poller|spot_runtime_activation|spot_dispatch_event|multi_socket_contract_regressions|zmp_request_reply)$|unittest_spot_data_plane_'
```

- build: 통과
- CTest: 12/12 통과

## 후보 perf 결과

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern PUBSUB,SPOT \
  --transports ws,wss \
  --duration 5 \
  --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round151_ws_wss_gather_capability_pubsub_spot
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_055103_round151_ws_wss_gather_capability_pubsub_spot.txt`
- runtime: `core/build`
- 시작 load average: `0.29 1.29 1.98`
- success 4, fail 0

| case | candidate | vs round149 | vs May26 full |
|------|-----------|-------------|---------------|
| `PUBSUB/ws` | 2,336,975.0 | +3.33% | +6.16% |
| `PUBSUB/wss` | 2,672,525.4 | -0.79% | -3.19% |
| `SPOT/ws` | 6,142,520.0 | +0.14% | +6.11% |
| `SPOT/wss` | 6,129,355.4 | +0.87% | -9.55% |

## 인접 제거 A/B

후보를 되돌린 뒤 다시 빌드하고 같은 범위를 측정했다.

```bash
cmake --build core/build --target libzlink -j$(nproc)
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern PUBSUB,SPOT \
  --transports ws,wss \
  --duration 5 \
  --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round151_ws_wss_gather_capability_removed_ab
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_060040_round151_ws_wss_gather_capability_removed_ab.txt`
- runtime: `core/build`
- 시작 load average: `1.16 2.41 2.36`
- success 4, fail 0

| case | candidate | removed A/B | candidate delta |
|------|-----------|-------------|-----------------|
| `PUBSUB/ws` | 2,336,975.0 | 2,332,057.4 | +0.21% |
| `PUBSUB/wss` | 2,672,525.4 | 2,693,792.6 | -0.79% |
| `SPOT/ws` | 6,142,520.0 | 6,026,555.4 | +1.92% |
| `SPOT/wss` | 6,129,355.4 | 5,996,164.6 | +2.22% |

## 판정

- 기각하고 되돌렸다.
- `SPOT/ws`와 `SPOT/wss`에는 작은 개선이 보였지만, 인접 제거 A/B에서 `PUBSUB/wss`가 -0.79% 하락했다.
- 사용자가 정한 "하락 항목 없이 플러스면 채택" 기준에는 맞지 않는다.
- POSD 관점에서도 capability 선언 자체는 단순하지만, 실제 의미가 "항상 gather write가 유리하다"까지
  보장되지 않는다면 transport가 engine에 전달하는 정책이 모호해진다. 작은 혼합 결과를 근거로 유지하지 않는다.
- 최종 source에는 이 후보를 남기지 않았다.
