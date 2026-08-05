# Round 149: PUBSUB/tls와 SPOT/wss worst refresh

## 목표

- retained SPOT SENDSEND fast path만 남긴 현재 source에서 May26 full 대비 가장 약한 64B 항목을 다시 확인한다.
- 이번 라운드 완료 기준:
  - current source와 `core/build` runtime이 맞는지 확인한다.
  - `PUBSUB tcp,tls,ws,wss`와 `SPOT tcp,tls,ws,wss` targeted perf를 같은 창에서 실행한다.
  - 재현되는 worst 항목이 있으면 core hot path 후보를 하나만 고른다.
  - 하락 없는 POSD-safe 후보가 없으면 source 변경 없이 근거를 남긴다.

## 기준 report

- corrected May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- retained reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_034903_round141_final_retained_spot_reduced_full.txt`
- latest STREAM/tcp guard:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_052439_round148_stream_current_pipe_defer_removed_ab.txt`

## 시작 상태

- source diff:
  - `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
  - `zlink_spot_send_spot_part()` 단일 `ZLINK_PART_FINAL` fast path만 유지한다.
- perf runner/client/server는 수정하지 않는다.

## May26 full 대비 round141 64B worst

| case | delta |
|------|------:|
| `MULTI_PUBSUB/tls` | -11.08% |
| `MULTI_SPOT/wss` | -9.42% |
| `MULTI_PUBSUB/tcp` | -4.70% |
| `MULTI_ROUTER_ROUTER/ws` | -2.89% |
| `MULTI_PUBSUB/wss` | -2.38% |

## 병목 가설

1. `PUBSUB/tls`는 round141 reduced full에서만 크게 낮았고, round142 targeted에서는 일부 회복했다.
   같은 창에서 `tcp,tls,ws,wss`를 다시 측정해 실제 worst인지 분리한다.
2. `SPOT/wss`는 round143 targeted에서도 May26 full 대비 약한 편이었다.
   SPOT publish ingress, poller refresh, handler allocator 후보는 이미 tcp/ws 하락 때문에 반려됐으므로
   재현 수치를 먼저 확인한다.
3. 두 항목이 모두 측정 편차로 완화되면, 새 source 후보를 넣지 않고 다음 후보는 ROUTER/DEALER 공통 경로로 넘긴다.

## 먼저 검증할 가설

- 가설 1과 2를 같은 run 조건에서 refresh한다.

## 실행

```bash
cmake --build core/build --target libzlink -j$(nproc)
sleep 45 && uptime && \
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern PUBSUB,SPOT --transports tcp,tls,ws,wss \
  --duration 5 --runs 7 --connect-ready-timeout-ms 5000 \
  --results-tag round149_pubsub_spot_worst_refresh
```

- build: pass
- 시작 load average: `0.35 1.01 1.68`
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_052834_round149_pubsub_spot_worst_refresh.txt`
- completion: success 8, fail 0

## 결과

May26 full 대비:

| case | throughput | delta |
|------|-----------:|------:|
| `MULTI_PUBSUB/tcp` | 2710752.4 | +1.85% |
| `MULTI_PUBSUB/tls` | 2397700.2 | -8.59% |
| `MULTI_PUBSUB/ws` | 2261698.0 | +2.74% |
| `MULTI_PUBSUB/wss` | 2693882.4 | -2.42% |
| `MULTI_SPOT/tcp` | 4146060.0 | +4.64% |
| `MULTI_SPOT/tls` | 6033697.0 | +1.58% |
| `MULTI_SPOT/ws` | 6133867.6 | +5.96% |
| `MULTI_SPOT/wss` | 6076363.2 | -10.33% |

round141 reduced full 대비:

| case | delta |
|------|------:|
| `MULTI_PUBSUB/tcp` | +6.87% |
| `MULTI_PUBSUB/tls` | +2.80% |
| `MULTI_PUBSUB/ws` | +3.79% |
| `MULTI_PUBSUB/wss` | -0.04% |
| `MULTI_SPOT/tcp` | +3.11% |
| `MULTI_SPOT/tls` | +0.32% |
| `MULTI_SPOT/ws` | -0.98% |
| `MULTI_SPOT/wss` | -1.01% |

## 판단

- `PUBSUB/tls`는 May26 full 대비 아직 `-8.59%`지만, round141의 `-11.08%`보다는 완화됐다.
- `PUBSUB/tcp/ws`는 May26 full보다 높고, `PUBSUB/wss`는 `-2.42%`라 큰 회귀로 보지 않는다.
- `SPOT/wss`는 May26 full 대비 `-10.33%`로 여전히 큰 회귀 경계에 있다.
- 다만 round141 대비로는 `-1.01%`라 현재 retained source가 새 하락을 만든 증거는 약하다.
- 기존 SPOT/WSS 개선 후보인 publish ingress move, handler allocator 확대, direct refresh 계열은
  tcp/ws 또는 SPOT_SENDSEND/WSS 하락 이력이 있어 그대로 재적용하지 않는다.
- 이번 round에서는 source 변경을 하지 않는다. 다음 검토는 `SPOT/wss`를 좁히되, tcp/ws 하락을 만들지
  않는 경로가 있는지 코드 기준으로 확인한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 현재까지 없음.
- 보안 의미를 유지한 근거:
  - mtrie 비재귀화, WS/WSS pending-copy 제거, 포트 파싱, IPC unlink 순서,
    decoder/message/send guard, `maxmsgsize` 정책을 수정하지 않는다.
- 추가로 실행한 회귀 테스트:
  - source 변경 없음. `cmake --build core/build --target libzlink -j$(nproc)`로 runtime 최신화만 확인했다.
