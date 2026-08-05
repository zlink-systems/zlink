# Round 144: dist/lb single-message helper recheck

## 목표

- 사용자가 제안한 기준에 맞춰, 과거에 개선폭이 작아 원복한 후보 중 다시 볼 가치가 있는 항목을 재검토한다.
- 대상은 round63의 `dist_t`/`lb_t` final single-message pipe helper 확장이다.
- 완료 기준:
  - current retained source와 같은 기준에서 one-way targeted를 비교한다.
  - 하락 항목이 없고 전체가 작게라도 개선되면 유지 후보로 둔다.
  - 하락 항목이 있으면 원복한다.

## 기준

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round142 one-way targeted current:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_042836_round142_oneway_targeted_current.txt`
- round143 SPOT/wss targeted current:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_044745_round143_spot_wss_targeted_current.txt`

## 재검토 이유

- round63 당시에는 평균 `+0.9%`, 중앙값 `+0.4%`라 원복했다.
- 현재 판단 기준은 `5%` 이상이면 명확한 개선이고, `1~2%`라도 하락 항목 없이 누적되는 개선이면
  채택 가능성을 둔다.
- 이 후보는 새 public API나 runner 변경 없이 기존 pipe helper를 재사용하므로, 성능 신호만 깨끗하면
  POSD 비용이 낮다.

## 위험

- round63에서는 problem 기준으로 일부 항목이 낮았고, 시작 load가 높았다.
- round99에서 `dist_t` 단독 적용도 `PUBSUB`에서 하락했다.
- 따라서 이번에는 `DEALER_DEALER,PUBSUB,SPOT` 전체 one-way targeted로 바로 하락 여부를 확인한다.

## 변경

- 변경 파일:
  - `core/src/runtime/sockets/internal/dist.cpp`
  - `core/src/runtime/sockets/internal/lb.cpp`
- 변경 내용:
  - final single-part이고 routing-id가 아닌 frame에서
    `pipe_t::write_single_message_and_flush_no_recursive_hwm_check()`를 사용한다.
  - multipart와 routing-id frame은 기존 경로를 유지한다.
- POSD 판단:
  - 새 상태나 public API를 추가하지 않고 기존 helper를 재사용한다.
  - 다만 성능 tradeoff가 있으면 일반 pipe send 경로에 예외 조건을 늘리는 비용을 정당화할 수 없다.

## Build와 focused tests

명령:

```bash
cmake --build core/build --target libzlink -j$(nproc) && \
ctest --test-dir core/build --output-on-failure \
  -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|spot_pubsub_scenario|backpressure_oneway_matrix|backpressure_matrix|multi_socket_contract_regressions)$'
```

결과:

- build: pass
- focused CTest: 5/5 pass

## Targeted perf

명령:

```bash
sleep 45 && uptime && \
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern DEALER_DEALER,PUBSUB,SPOT \
  --transports tcp,tls,ws,wss \
  --duration 5 --runs 5 --connect-ready-timeout-ms 5000 \
  --results-tag round144_dist_lb_single_message_recheck
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_045424_round144_dist_lb_single_message_recheck.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `0.34 1.31 1.99`
- 완료: success 12, fail 0

round142 one-way targeted current 대비:

| item | candidate | round142 | delta |
|------|----------:|---------:|------:|
| MULTI_DEALER_DEALER/tcp | 3071071.4 | 3080658.8 | -0.31% |
| MULTI_DEALER_DEALER/tls | 3245407.6 | 3194117.2 | +1.61% |
| MULTI_DEALER_DEALER/ws | 3160460.2 | 3176448.2 | -0.50% |
| MULTI_DEALER_DEALER/wss | 3340791.2 | 3333643.2 | +0.21% |
| MULTI_PUBSUB/tcp | 2724370.2 | 2582328.0 | +5.50% |
| MULTI_PUBSUB/tls | 2375079.8 | 2392173.4 | -0.71% |
| MULTI_PUBSUB/ws | 2296499.8 | 2249374.4 | +2.10% |
| MULTI_PUBSUB/wss | 2682612.0 | 2663428.8 | +0.72% |
| MULTI_SPOT/tcp | 3992320.0 | 3983980.0 | +0.21% |
| MULTI_SPOT/tls | 5780960.4 | 6010403.8 | -3.82% |
| MULTI_SPOT/ws | 6103044.6 | 5981994.2 | +2.02% |
| MULTI_SPOT/wss | 5954963.2 | 6023719.8 | -1.14% |

요약:

- average: `+0.49%`
- median: `+0.21%`
- worst: `MULTI_SPOT/tls -3.82%`
- best: `MULTI_PUBSUB/tcp +5.50%`

## 판정

- 일부 항목은 좋아졌지만 하락 항목이 있다.
- 사용자가 제안한 `하락 항목 없이 +` 기준을 만족하지 못한다.
- 평균과 중앙값도 작아, 일반 pipe send 경로에 예외 조건을 남길 근거가 약하다.
- 후보는 원복한다.

## 원복

- 원복 파일:
  - `core/src/runtime/sockets/internal/dist.cpp`
  - `core/src/runtime/sockets/internal/lb.cpp`
- 원복 후 명령:

```bash
cmake --build core/build --target libzlink -j$(nproc) && \
git diff -- core/src/runtime/sockets/internal/dist.cpp \
  core/src/runtime/sockets/internal/lb.cpp \
  core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp
```

결과:

- build: pass
- `dist.cpp`, `lb.cpp` diff 없음
- retained source diff는 `zlink_spot_send_spot_part()` 단일 FINAL fast path뿐이다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음.
- 보안 의미를 유지한 근거:
  - 최종 source는 후보를 원복한 상태다.
  - mtrie 비재귀화, WS/WSS pending-copy 제거, 포트 파싱, IPC unlink 순서, decoder/message/send guard,
    `maxmsgsize` 정책을 변경하지 않는다.
