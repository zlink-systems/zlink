# Round 108: retained SPOT fast path broad guard

## 이번 라운드 목표

- 현재 유지 중인 `zlink_spot_send_spot_part()` FINAL-only fast path가 SPOT focused set 밖에서 하락을 만들지 않는지 확인한다.
- 완료 기준:
  - core runtime이 원복 후 source와 맞게 빌드되어 있다.
  - `PUBSUB,SPOT_SENDSEND,SPOT_REQREP,STREAM` 64B tcp/tls/wss focused set에서 실패 0개를 확인한다.
  - 하락 항목이 크면 retained 변경도 재검토한다.

## 기준 report

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round100 reduced stability:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_155904_round100_reduced_stream_transition_stability.txt`
- round105 SPOT recheck:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_163053_round105_spot_retained_fastpath_recheck.txt`
- round106, round107:
  - 두 후보 모두 하락 항목이 있어 source를 되돌렸다.

## 시작 상태

- retained source diff:
  - `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
  - `zlink_spot_send_spot_part()`에서 single FINAL이고 active send sequence가 없을 때 기존 `spot_send_spot_impl()`로 바로 위임한다.
- round106 ASIO allocator 확대: 폐기 및 원복.
- round107 SPOT external route id parse elision: 폐기 및 원복.
- perf 전용 변경: 없음.

## 병목 가설

1. retained SPOT fast path는 SPOT_SENDSEND에서는 staging 비용을 줄이지만, shared core runtime 변화가 아니므로 PUBSUB/STREAM에는 직접 영향이 없어야 한다.
2. 최근 WSS/STREAM/REQREP 수치 변동이 크므로, 단일 SPOT focused 결과만으로 retained 변경을 과대평가할 수 있다.
3. 새 코드 후보를 추가하기 전, 현재 기준 runtime이 넓은 focused set에서 실패 없이 안정적인지 확인해야 다음 후보 판단이 흐려지지 않는다.

## 먼저 검증할 가설

- H1: retained 변경만 남긴 runtime에서 `PUBSUB,SPOT_SENDSEND,SPOT_REQREP,STREAM` 64B tcp/tls/wss가 실패 0개로 완료되는가.
- H2: SPOT_SENDSEND 효과가 유지되면서 PUBSUB/STREAM에 큰 하락이 없는가.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거를 되돌리지 않는다.
  - mtrie 비재귀화, port parsing, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 수정하지 않는다.
  - perf 조건이나 runner/client/server 코드를 바꾸지 않는다.

## 실행 예정

```bash
cmake --build core/build -j$(nproc)
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB,SPOT_SENDSEND,SPOT_REQREP,STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round108_retained_spot_broad_guard
```

## 실행

- build: pass
- perf status: complete, fail 0
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_170320_round108_retained_spot_broad_guard.txt`
- load_avg: `2.99 12.63 11.77`

## 결과

| pattern | transport | May26 full | round100 | round105 | round108 |
|---------|-----------|------------|----------|----------|----------|
| MULTI_PUBSUB | tcp | 2844777.8 | 2368520.0 | n/a | 2325023.0 |
| MULTI_PUBSUB | tls | 2623065.0 | 2216652.6 | n/a | 2205751.4 |
| MULTI_PUBSUB | wss | 2760571.0 | 2465419.0 | n/a | 2432262.8 |
| MULTI_SPOT_SENDSEND | tcp | 271206.0 | 253359.0 | 256003.6 | 239659.4 |
| MULTI_SPOT_SENDSEND | tls | 254009.6 | 249998.2 | 251436.6 | 229154.0 |
| MULTI_SPOT_SENDSEND | wss | 252557.8 | 244308.4 | 252776.8 | 237525.6 |
| MULTI_SPOT_REQREP | tcp | 252212.6 | 252806.8 | 255914.6 | 243765.2 |
| MULTI_SPOT_REQREP | tls | 229720.4 | 223681.2 | 232934.0 | 222628.2 |
| MULTI_SPOT_REQREP | wss | 219301.2 | 214421.2 | 210854.2 | 207444.6 |
| MULTI_STREAM | tcp | n/a | 280166.0 | n/a | 268866.4 |
| MULTI_STREAM | tls | n/a | 209046.0 | n/a | 186611.4 |
| MULTI_STREAM | wss | 184722.2 | 175863.2 | n/a | 163031.8 |

## 판단

- 실패 0개는 확인했다.
- 하지만 round108은 SPOTSEND, SPOTREQREP, STREAM이 전반적으로 낮게 나와 retained fast path의 broad guard로는 약한 근거다.
- SPOTSEND뿐 아니라 retained fast path 영향 대상이 아닌 SPOTREQREP/STREAM도 같이 낮아졌으므로, 이 run 하나만으로 retained SPOT fast path를 폐기하지 않는다.
- round105의 SPOT focused recheck는 retained fast path가 SPOTSEND tcp/tls/wss 모두 round100보다 높았던 더 직접적인 근거다.
- 다음 판단:
  - 새 코드를 추가하기 전, 가능하면 낮은 load에서 SPOTSEND focused A/B 또는 재측정을 한 번 더 해야 한다.
  - STREAM/tls,wss는 round108에서 특히 낮으므로, 다음 후보는 STREAM만이 아니라 현재 측정 환경/transport 변동과 ASIO path를 분리해 봐야 한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거를 되돌리지 않았다.
  - mtrie 비재귀화, port parsing, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 수정하지 않았다.
  - perf runner/client/server를 수정하지 않았다.
