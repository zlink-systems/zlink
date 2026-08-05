# Round 123: PUBSUB tls/ws hot path 후보 선정

## 이번 라운드 목표

- Round 122 low-load all64에서 problem report 대비 거의 개선되지 않은 `MULTI_PUBSUB tls/ws 64B`를 좁힌다.
- perf runner/client/server는 수정하지 않는다.
- core runtime hot path에 의미가 있고 POSD를 해치지 않는 후보만 source 변경으로 검증한다.

## 기준 report

- problem report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- corrected full baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- current low-load all64:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_223125_round122_lowload_all64_reduced_full.txt`

## 시작 상태

- `core/src`, `core/include`, `core/tests`: source diff 없음.
- perf runner/client/server는 수정하지 않는다.
- Round 122 low-load all64:
  - status: complete
  - fail: 0
  - 전체 64B problem 대비 평균: `+7.05%`
  - 전체 64B problem 대비 중앙값: `+4.76%`
  - 미달 목표: 전체 평균 `+8%`, 전체 중앙값 `+10%`

## 병목 가설

- 가설 1:
  PUBSUB 64B non-VSM 메시지 fanout에서 `msg_t::add_refs()` / `rm_refs()` refcount 비용이 TLS/WS 출력 경로와 결합해 중앙값을 누른다.
- 가설 2:
  PUBSUB matching은 이미 empty subscription steady state에서 root pipe set만 방문하므로, mtrie traversal보다 ASIO/ZMTP encoder output path가 더 크다.
- 가설 3:
  TLS/WS transport 자체는 speculative/gather 제약이 있어, socket 공통 output policy를 바꾸면 다른 transport 하락 위험이 크다.

## 먼저 검증할 가설

- 가설 1과 2를 코드로 확인한다.
  - `msg_t` VSM 기준과 refcount 경로를 확인한다.
  - `dist_t::distribute()` non-VSM fanout 경로를 확인한다.
  - `asio_zmp_engine_t` output/gather 조건을 확인한다.

## 코드 확인

- `core/src/runtime/core/msg.hpp`
  - `msg_t::msg_t_size == 64`
  - `max_vsm_size == msg_t_size - (3 + 16 + sizeof(uint32_t))`
  - 현재 layout에서 `max_vsm_size`는 41B이므로 64B payload는 VSM이 아니라 LMSG다.
- `core/src/runtime/core/msg.cpp`
  - 64B LMSG fanout은 `add_refs()`에서 shared refcount를 세팅하고,
    downstream 실패가 있으면 `rm_refs()`로 되돌린다.
- `core/src/runtime/sockets/internal/dist.cpp`
  - VSM은 add_refs 없이 matching pipe에 직접 write한다.
  - 64B LMSG는 `_matching - 1` refs를 추가한 뒤 matching pipe에 복사된 `msg_t` header를 쓴다.
- `core/src/runtime/engine/asio/asio_zmp_engine.cpp`
  - ZMP gather header builder는 있으나, 일반 gather는 기본으로 켜져 있지 않고 threshold도 64B에는 적용되지 않는다.
- `core/src/runtime/transports/tls/ssl_transport.hpp`
  - TLS transport는 `supports_speculative_write() == false`이고 gather-write도 지원하지 않는다.
- `core/src/runtime/transports/ws/ws_transport.hpp` / `core/src/runtime/transports/tls/wss_transport.hpp`
  - WS/WSS도 speculative/gather write를 기본 지원하지 않는다.

## gather threshold probe

- command:
  `ZLINK_ASIO_GATHER_WRITE=1 ZLINK_ASIO_GATHER_THRESHOLD=64 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round123_pubsub_gather64_probe`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_230536_round123_pubsub_gather64_probe.txt`
- status: complete
- fail: 0
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`

| transport | gather64 probe kops | round122 current kops | delta |
|-----------|---------------------|-----------------------|-------|
| tcp | 2767.234 | 2752.253 | +0.54% |
| tls | 2413.122 | 2438.543 | -1.04% |
| ws | 2306.856 | 2197.126 | +4.99% |
| wss | 2716.094 | 2687.476 | +1.06% |

## 판단

- gather64 probe는 `ws`에 약 +5% 신호가 있지만 `tls`가 하락한다.
- 일반 ZMP gather를 기본 켜거나 threshold를 64로 낮추는 변경은 transport별 하락 없이 개선한다는 기준을 만족하지 않는다.
- TLS/WS transport에 speculative/gather를 새로 켜는 방식은 과거 round80/round106에서 이미 하락 또는 blocking 위험으로 배제된 판단과 충돌한다.
- `msg_t` VSM 한계를 64B까지 늘리는 것은 public `zlink_msg_t` 크기와 내부 group/routing layout을 건드리는 큰 ABI/설계 변경이라 이번 perf 후보로 부적절하다.
- source 변경을 남기지 않는다.

## 다음 후보

- PUBSUB 64B는 dist/mtrie/ASIO gather 쪽에서 현재 채택 가능한 후보가 없다.
- 다음 후보는 `SPOT_SENDSEND tcp/wss`의 May26 대비 약한 하락을 보되, round92에서 유지/폐기된 fast path와 겹치지 않는지 먼저 확인한다.
