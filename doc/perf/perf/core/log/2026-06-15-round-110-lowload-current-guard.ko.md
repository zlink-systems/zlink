# Round 110: low-load current guard

## 이번 라운드 목표

- source diff가 없는 clean core runtime에서 낮은 load의 STREAM/PUBSUB 64B 기준을 다시 확보한다.
- 완료 기준:
  - `core/build` runtime을 재확인한다.
  - `PUBSUB,STREAM` 64B tcp/tls/wss focused perf가 실패 0개로 완료된다.
  - 결과를 round98/round100/round108과 비교해 다음 코드 후보의 판단 기준으로 삼는다.

## 기준 report

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round98 STREAM-only:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_154759_round98_stream_transport_transition_failure_recheck.txt`
- round100 reduced:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_155904_round100_reduced_stream_transition_stability.txt`
- round108 broad guard:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_170320_round108_retained_spot_broad_guard.txt`

## 시작 상태

- core source diff: 없음.
- perf 전용 변경: 없음.
- 최근 판단:
  - SPOT part fast path는 5% 미만 효과라 제거했다.
  - ASIO output loop refactor, STREAM complete-frame fast path, initial target cap, tiny gather threshold는 이미 폐기했다.

## 병목 가설

1. round108/round109는 load가 높아 STREAM/SPOT 수치가 전반적으로 낮았다. 낮은 load에서 clean current를 다시 측정해야 다음 후보의 신호를 분리할 수 있다.
2. STREAM/tls,wss 하락은 ASIO/TLS/WSS transport transition이나 read/write target 정책의 변동일 수 있다.
3. PUBSUB는 core source 변경과 무관하게 May26 대비 낮게 유지되는지, 아니면 측정 환경 변동인지 재확인이 필요하다.

## 먼저 검증할 가설

- H1: 낮은 load에서 clean current `PUBSUB,STREAM` 64B tcp/tls/wss가 실패 없이 완료되는가.
- H2: STREAM/tcp가 300k대, STREAM/tls/wss가 round98/round100 수준으로 복귀하는가.
- H3: PUBSUB/tcp,tls,wss가 round100 수준을 유지하는가.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음. source 변경 없이 측정만 수행한다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거, mtrie 비재귀화, port parsing, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 수정하지 않는다.
  - perf runner/client/server를 수정하지 않는다.

## 실행 예정

```bash
cmake --build core/build -j$(nproc)
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB,STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round110_lowload_current_guard
```

## 실행

- build: pass
- perf status: complete, fail 0
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_171900_round110_lowload_current_guard.txt`
- start load_avg: `0.92 5.05 7.81`

## 결과

| pattern | transport | May26 full | round98 | round100 | round108 | round110 |
|---------|-----------|------------|---------|----------|----------|----------|
| MULTI_PUBSUB | tcp | 2844777.8 | n/a | 2368520.0 | 2325023.0 | 2357759.4 |
| MULTI_PUBSUB | tls | 2623065.0 | n/a | 2216652.6 | 2205751.4 | 2153328.0 |
| MULTI_PUBSUB | wss | 2760571.0 | n/a | 2465419.0 | 2432262.8 | 2445626.8 |
| MULTI_STREAM | tcp | n/a | 304673.8 | 280166.0 | 268866.4 | 252199.0 |
| MULTI_STREAM | tls | n/a | 185889.0 | 209046.0 | 186611.4 | 184091.4 |
| MULTI_STREAM | wss | 184722.2 | 177304.6 | 175863.2 | 163031.8 | 157238.8 |

## 판단

- 실패 0개와 `core/build` runtime은 확인했다.
- PUBSUB는 round100 근처지만 May26 full보다 여전히 낮다.
- STREAM은 낮은 load에서도 tcp/tls/wss가 모두 낮다.
  - tcp는 round98 `304673.8`보다 `-17.22%`.
  - wss는 May26 full `184722.2`보다 `-14.88%`.
- round108의 낮은 STREAM 수치가 단순 고부하 때문만은 아니다.
- 다음 후보는 STREAM dispatch/ASIO path를 우선으로 좁힌다. 다만 이미 폐기한 complete-frame parser fast path, initial target cap, tiny gather threshold, output loop refactor는 반복하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음. source 변경 없이 측정만 수행했다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거, mtrie 비재귀화, port parsing, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 수정하지 않았다.
  - perf runner/client/server를 수정하지 않았다.
