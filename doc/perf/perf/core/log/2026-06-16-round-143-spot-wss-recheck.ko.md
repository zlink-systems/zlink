# Round 143: SPOT wss recheck

## 목표

- May26 full 기준으로 `MULTI_SPOT/wss 64B`가 `-10%` 근처에서 흔들리는 원인을 다시 분리한다.
- 기존에 하락 항목 때문에 원복한 SPOT publish ingress 후보를 그대로 반복하지 않는다.
- 완료 기준:
  - source 변경 전 targeted `SPOT wss 64B`를 낮은 부하에서 재측정한다.
  - 반복 하락이 확인될 때만 core-only 최소 후보를 검토한다.
  - 후보는 `SPOT tcp,tls,ws,wss`에서 하락 없이 개선될 때만 유지한다.

## 기준

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round122 reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_214654_round122_current_reduced_full_after_stream_revert.txt`
- round134 retained SPOT reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_014400_round134_current_retained_spot_reduced_full.txt`
- round135 SPOT targeted current:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_022344_round135_spot_wss_targeted_current.txt`
- round141 final retained SPOT reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_034903_round141_final_retained_spot_reduced_full.txt`
- round142 one-way targeted current:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_042836_round142_oneway_targeted_current.txt`

## 시작 상태

- 유지 source diff:
  - `zlink_spot_send_spot_part()` 단일 `ZLINK_PART_FINAL` fast path
- perf runner/client/server는 수정하지 않는다.
- 보안 하드닝 항목은 수정하지 않는다.

## 기존 증거

- round135 targeted:
  - `MULTI_SPOT/wss 64B`: `6100341.2`
  - May26 full 대비 `-9.98%`
  - round134 대비 `+2.26%`
- round141 reduced full:
  - `MULTI_SPOT/wss 64B`: `6138139.0`
  - May26 full 대비 `-9.42%`
  - round134 대비 `+2.89%`
- round142 one-way targeted:
  - `MULTI_SPOT/wss 64B`: `6023719.8`
  - May26 full 대비 `-11.11%`
  - round141 대비 `-1.86%`
- round74 old/current standalone에서는 `SPOT/wss`가 May26 근처까지 회복된 적이 있어,
  run-order/load 변동 가능성이 남아 있다.

## 재검토한 후보

- round119 `SPOT publish ingress move`:
  - WSS와 TLS는 크게 올렸지만 TCP와 WS를 낮췄다.
  - revert A/B 기준 후보 delta:
    - tcp `-7.93%`
    - tls `+17.65%`
    - ws `-9.09%`
    - wss `+12.99%`
  - 하락 항목이 있으므로 그대로 재적용하지 않는다.
- WSS handler allocator 후보:
  - 일부 WSS 항목은 올렸지만 `SPOT_SENDSEND/wss`, `SPOT_REQREP/wss` 하락이 있어 원복됐다.
- staged ingress/direct refresh 계열:
  - non-tcp 일부를 올렸지만 tcp/ws tradeoff가 있어 원복됐다.

## 병목 가설

1. `SPOT/wss` gap은 retained source diff의 직접 회귀가 아니라 긴 실행 순서와 WSS transport 변동일 수 있다.
2. publish ingress의 ownership/copy 비용은 WSS에서 보이지만, 단순 move 후보는 TCP/WS의 queue timing을 해친다.
   같은 의미를 유지하면서 특정 transport만 유리하게 만드는 상태를 추가하면 POSD상 복잡성이 커진다.
3. WSS transport 출력 경로 후보는 이미 다른 SPOT 계열 하락이 확인됐으므로, 재적용하려면 더 좁은 근거가 필요하다.

## 먼저 검증할 가설

- source 변경 없이 `SPOT wss 64B` targeted를 낮은 부하에서 다시 실행한다.
- targeted가 May26 대비 `-10%` 안쪽으로 회복되면 source 변경을 하지 않는다.
- 반복해서 `-10%` 밖이면 SPOT data-plane과 WSS 경로를 다시 읽되, tcp/ws 하락을 부르는 후보는 배제한다.

## source 변경 전 targeted 측정

명령:

```bash
cmake --build core/build --target libzlink -j$(nproc) && sleep 45 && uptime && \
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern SPOT --transports wss \
  --duration 5 --runs 7 --connect-ready-timeout-ms 5000 \
  --results-tag round143_spot_wss_targeted_current
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_044745_round143_spot_wss_targeted_current.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `0.58 1.61 2.25`
- 완료: success 1, fail 0
- `MULTI_SPOT/wss 64B`: `6025834.8`

원시 run:

| run | kmsg/s |
|-----|-------:|
| 1 | 5445.100 |
| 2 | 5962.394 |
| 3 | 6108.339 |
| 4 | 3630.060 |
| 5 | 6025.835 |
| 6 | 6050.533 |
| 7 | 6042.653 |

비교:

| 기준 | delta |
|------|------:|
| May26 full | -11.07% |
| round122 | -1.22% |
| round134 | +1.01% |
| round135 targeted | -1.22% |
| round141 | -1.83% |
| round142 | +0.04% |

판단:

- 최근 current 계열인 round142와 거의 같은 값이다.
- May26 full 기준 gap은 반복되지만, 최근 retained source diff의 새 회귀라기보다는 May26 full과 현재 실행군의
  기준 차이 또는 WSS run 변동으로 보는 쪽이 더 타당하다.
- 단일 run 중 `3630.060 Kmsg/s` outlier가 있어 WSS targeted 자체도 tail 변동이 크다.

## 코드 triage

- SPOT data-plane publish 경로는 이미 다음 순서다.
  - caller thread에서 publish ingress queue에 복사 후 enqueue
  - data-plane thread가 queue를 drain
  - pending/staged가 없으면 local fanout과 mesh publish를 바로 시도
  - `EAGAIN`일 때만 pending/staged queue로 복사
- 복사 횟수를 줄이는 ownership move 후보는 round119에서 이미 검증됐다.
  - WSS/TLS는 올랐지만 TCP/WS를 낮춰 채택하지 않았다.
- `refresh_poller_interest()` 성공 경로 축소도 후보처럼 보이지만, round33 local fanout refresh guard 계열이
  불안정했고 `SPOT/wss` 큰 하락을 만들었다. 같은 계열을 반복하지 않는다.

## 현재 판단

- `SPOT/wss` May26 full gap만 보고 transport별 분기나 publish entry ownership 상태를 추가하면 POSD상
  인터페이스는 그대로여도 내부 상태와 예외 경로가 늘어난다.
- tcp/ws 하락 없이 이 항목만 좁히는 core-only 후보가 현재 코드에서 명확하지 않다.
- 이번 round에서는 source 변경을 하지 않는다.
- 다음 후보는 `DEALER_DEALER`와 `PUBSUB`가 공유하는 pipe enqueue/flush, mailbox/wakeup, poller 경로에서
  찾는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. source 변경 없이 측정과 코드 triage만 수행했다.
- 보안 의미를 유지한 근거:
  - mtrie 비재귀화, WS/WSS pending-copy 제거, 포트 파싱, IPC unlink 순서, decoder/message/send guard,
    `maxmsgsize` 정책을 변경하지 않는다.
