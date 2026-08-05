# Round 125: SPOT_SENDSEND FINAL-only fast path 재검토

## 이번 라운드 목표

- round92에서 효과가 있었지만 round109에서 `5% 미만` 기준 때문에 제거된
  `zlink_spot_send_spot_part()` FINAL-only fast path를 새 기준으로 재검토한다.
- 새 기준:
  - 하락 항목 없이 작은 개선이면 채택 가능 후보로 본다.
  - 하락이 반복되거나 효과가 없으면 source 변경을 되돌린다.

## 기준 report

- corrected full baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- corrected smoke baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- current low-load all64:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_223125_round122_lowload_all64_reduced_full.txt`

## 재검토 이유

- round92:
  - `SPOT_SENDSEND/tcp,tls,wss`가 모두 round90 current보다 상승했다.
  - tcp는 `+6.86%`, tls는 `+3.43%`, wss는 `+1.97%`.
- round109:
  - high-load same-window A/B에서 retained fast path가 tls/wss에서 더 높고 tcp는 사실상 동률이었다.
  - 당시 판정은 `5% 미만은 미채택`이었다.
- 사용자가 이번 기준을 `하락 항목 없이 +면 채택 가능`으로 조정했으므로, 이 후보는 재검토 대상이다.

## 적용한 변경

- `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
  - `zlink_spot_send_spot_part()`에서 `part_flag_ == ZLINK_PART_FINAL`이고 해당 handle에 열린
    send sequence가 없으면 `spot_send_spot_impl()`을 직접 호출한다.
  - multipart sequence가 열려 있거나 `PART_MORE`이면 기존 staged sequence 경로를 그대로 쓴다.
  - backend 실패 시 기존 staged path와 맞게 caller part를 소비한다.

## POSD 검토

- 단일 FINAL 메시지는 multipart 상태 머신이 필요 없는 별도 의미다.
- public API와 contract를 늘리지 않는다.
- 상태 머신을 우회하는 조건은 함수 내부에 갇혀 있고 호출자에게 새 전제 조건을 만들지 않는다.
- request/reply pending 경로까지 넓히지 않고, 이전에 효과가 있었던 send-spot echo 경로에만 제한한다.

## 보안 하드닝 보존 확인

- WS/WSS pending message 전체 사본 제거, mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서,
  decoder/message/send guard, maxmsgsize 정책을 수정하지 않는다.
- perf runner/client/server는 수정하지 않는다.

## 기능 검증

- `cmake --build core/build -j$(nproc)`
  - 첫 시도는 `zlink_submit_result_t`를 struct처럼 읽은 실수로 실패했다.
  - `result != ZLINK_SUBMIT_OK` 판정으로 고친 뒤 재실행은 통과했다.
- `ctest --test-dir core/build --output-on-failure -R 'spot|zmp_request_reply|request_reply'`
  - 38/38 통과.

## SPOT_SENDSEND targeted perf

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT_SENDSEND --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round125_spot_sendsend_final_fastpath_recheck`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_232231_round125_spot_sendsend_final_fastpath_recheck.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,16.79 10.38 6.60`
- 결과:
  - tcp: `266354.0`
  - tls: `230352.6`
  - wss: `244813.0`
- 판단:
  - tcp는 높지만 tls/wss가 낮다.
  - 시작 부하가 높아 판정에는 쓰지 않고 재측정한다.

- 재측정 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT_SENDSEND --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round125_spot_sendsend_final_fastpath_recheck_rerun`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_232519_round125_spot_sendsend_final_fastpath_recheck_rerun.txt`
- 시작 부하:
  `load_avg,4.23 7.48 6.12`
- 결과:
  - tcp: `259731.0`
  - tls: `243646.0`
  - wss: `248810.6`
- 판단:
  - round122 current 대비로는 tls/wss가 낮다.
  - 같은 시간대 자체가 낮은지 구분하기 위해 fast path를 제거하고 원복 A/B를 실행한다.

## 원복 A/B

- fast path를 제거하고 `cmake --build core/build --target libzlink -j$(nproc)` 실행.
  - 통과.
- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT_SENDSEND --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round125_spot_sendsend_final_fastpath_removed_ab`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_232818_round125_spot_sendsend_final_fastpath_removed_ab.txt`
- 시작 부하:
  `load_avg,4.46 5.93 5.74`

| transport | fast path | removed A/B | fast path delta |
|-----------|----------:|------------:|----------------:|
| tcp | 259731.0 | 250451.6 | +3.70% |
| tls | 243646.0 | 229433.8 | +6.19% |
| wss | 248810.6 | 242016.0 | +2.81% |

## 현재 판정

- same-window A/B에서는 세 transport 모두 fast path가 우세하다.
- 효과는 일부 5% 미만이지만 하락 항목은 없고, tls는 5% 이상이다.
- source를 fast path 상태로 다시 적용하고 `cmake --build core/build --target libzlink -j$(nproc)`를 통과시켰다.
- 최종 채택 전 인접 `SPOT_SENDSEND,SPOT_REQREP` guard를 실행한다.

## 인접 guard

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT_SENDSEND,SPOT_REQREP --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round125_spot_sendsend_final_fastpath_adjacent_guard`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_233148_round125_spot_sendsend_final_fastpath_adjacent_guard.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,3.33 4.66 5.27`
- status:
  complete, fail 0.

| pattern | transport | throughput |
|---------|-----------|-----------:|
| SPOT_SENDSEND | tcp | 253138.6 |
| SPOT_SENDSEND | tls | 244124.4 |
| SPOT_SENDSEND | wss | 255475.8 |
| SPOT_REQREP | tcp | 271202.0 |
| SPOT_REQREP | tls | 235415.2 |
| SPOT_REQREP | wss | 227875.4 |

### guard 판단

- `SPOT_REQREP`는 May26 full 대비 tcp/tls/wss가 모두 높다.
- `SPOT_SENDSEND`는 guard run에서 tcp/tls가 round122 current보다 낮지만, 바로 앞 same-window 원복 A/B에서
  fast path가 tcp/tls/wss 모두 원복보다 높았다.
- 이 guard는 pattern 순서와 측정 창 영향이 섞였으므로, fast path 효과 판정은 원복 A/B를 우선한다.
- 다음 단계는 전체 64B reduced full을 다시 실행해 목표 평균/중앙값에 실제로 기여하는지 확인한다.

## low-load all64 reduced full

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round125_spot_sendsend_final_fastpath_lowload_all64_reduced_full`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_233726_round125_spot_sendsend_final_fastpath_lowload_all64_reduced_full.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,2.15 3.35 4.53`
- status:
  complete, fail 0.

### problem report 대비

- common 64B throughput 항목: 26개
- 전체 평균: `+5.80%`
- 전체 중앙값: `+4.82%`
- one-way 평균: `+5.72%`
- one-way 중앙값: `+0.97%`
- echo 평균: `+5.86%`
- echo 중앙값: `+6.02%`

worst:

| item | current | problem | delta |
|------|--------:|--------:|------:|
| MULTI_STREAM/wss | 160.401 kops | 182.406 kops | -12.06% |
| MULTI_PUBSUB/ws | 2053.910 kops | 2220.372 kops | -7.50% |
| MULTI_PUBSUB/tcp | 2488.606 kops | 2628.105 kops | -5.31% |
| MULTI_PUBSUB/tls | 2416.785 kops | 2446.708 kops | -1.22% |
| MULTI_PUBSUB/wss | 2660.030 kops | 2679.903 kops | -0.74% |

### round122 current 대비

- common 64B throughput 항목: 32개
- 전체 평균: `-1.38%`
- 전체 중앙값: `-0.75%`
- one-way 평균: `-2.50%`
- one-way 중앙값: `-0.96%`
- echo 평균: `-0.71%`
- echo 중앙값: `-0.07%`

worst:

| item | round125 | round122 | delta |
|------|---------:|---------:|------:|
| MULTI_STREAM/wss | 160.401 kops | 189.070 kops | -15.16% |
| MULTI_STREAM/ws | 251.775 kops | 281.723 kops | -10.63% |
| MULTI_PUBSUB/tcp | 2488.606 kops | 2752.253 kops | -9.58% |
| MULTI_PUBSUB/ws | 2053.910 kops | 2197.126 kops | -6.52% |
| MULTI_SPOT/ws | 5751.162 kops | 6053.862 kops | -5.00% |

### SPOT_SENDSEND focused item

| baseline | tcp | tls | ws | wss |
|----------|----:|----:|---:|----:|
| vs round122 | +0.62% | -0.41% | +0.53% | -1.67% |
| vs problem | +5.44% | +5.46% | n/a | n/a |
| vs May26 full | -3.59% | -2.01% | +0.64% | -0.01% |
| vs May26 smoke | -0.98% | +0.76% | -0.08% | -8.90% |

## 최종 판정

- source 변경은 유지한다.
- 근거:
  - same-window 원복 A/B에서 fast path가 tcp/tls/wss 모두 우세했다.
  - 효과는 tcp `+3.70%`, tls `+6.19%`, wss `+2.81%`였다.
  - 인접 guard는 fail 0이고 `SPOT_REQREP`는 May26 full 대비 tcp/tls/wss 모두 높았다.
  - 전체 reduced full의 목표 미달은 주로 `PUBSUB`와 `STREAM/wss` 변동이며, 이번 source 변경이 닿는
    `SPOT_SENDSEND` 직접 효과와 분리해서 봐야 한다.
- 남은 문제:
  - 전체 problem 대비 평균/중앙값 목표는 아직 미달이다.
  - 다음 후보는 `PUBSUB/tcp,ws`와 `STREAM/wss` 변동을 같은 시간대 A/B 방식으로 다시 좁힌다.
