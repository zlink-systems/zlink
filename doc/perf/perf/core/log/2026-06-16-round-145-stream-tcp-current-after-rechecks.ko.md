# Round 145: STREAM tcp current after rechecks

## 목표

- round144 후보 원복 뒤 retained source 상태에서 `STREAM/tcp 64B` 현재 수치를 다시 확인한다.
- 400kops 목표와 corrected May26 full 기준을 분리해 판단한다.
- 기존에 하락으로 배제한 STREAM 후보를 반복하지 않는다.

## 시작 상태

- 유지 source diff:
  - `zlink_spot_send_spot_part()` 단일 `ZLINK_PART_FINAL` fast path
- round144 `dist/lb` single-message helper 후보는 원복했다.
- perf runner/client/server는 수정하지 않는다.
- 보안 하드닝 항목은 수정하지 않는다.

## 기준

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round140 current:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_034212_round140_stream_current_after_tls_revert.txt`
- round141 reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_034903_round141_final_retained_spot_reduced_full.txt`

## 재검토 제외 후보

- current pipe 조회 중복 제거: tcp/ws 일부 개선 신호가 있었지만 wss 하락으로 원복.
- `_dispatch_inflight` 제거: 네 전송 모두 낮아 원복.
- TCP tiny gather: tcp/ws는 좋아졌지만 wss 하락으로 원복.
- packet complete-frame fast path: parser 복잡도 증가 대비 tcp 개선 없음.
- type check elision: 효과 없음.
- perf helper `send_mutex` 제거: 379K~386K 진단 신호가 있었지만 perf code 변경이라 이번 core-only 범위 밖.

## current 측정 1

명령:

```bash
sleep 45 && uptime && \
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern STREAM --transports tcp \
  --duration 5 --runs 7 --connect-ready-timeout-ms 5000 \
  --results-tag round145_stream_tcp_current_after_rechecks
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_051148_round145_stream_tcp_current_after_rechecks.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `2.04 2.98 2.70`
- 완료: success 1, fail 0
- `MULTI_STREAM/tcp 64B`: `309329.8`

판단:

- 시작 load가 round140보다 높아 직접 판정에는 약하다.
- 낮은 부하를 기다린 뒤 다시 측정한다.

## current 측정 2

명령:

```bash
sleep 90 && uptime && \
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern STREAM --transports tcp \
  --duration 5 --runs 7 --connect-ready-timeout-ms 5000 \
  --results-tag round145_stream_tcp_current_lowload_retry
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_051333_round145_stream_tcp_current_lowload_retry.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `1.28 2.45 2.54`
- 완료: success 1, fail 0
- `MULTI_STREAM/tcp 64B`: `316315.0`

비교:

| 기준 | delta |
|------|------:|
| May26 full | +3.65% |
| round140 current | -1.77% |
| round141 reduced full | -6.82% |
| 400kops target | -20.92% |

## 판단

- corrected May26 full 기준으로는 여전히 플러스다.
- 400kops 목표에는 약 `84kops` 부족하다.
- 현재까지 core-only STREAM 후보 중 하락 없이 반복 개선된 항목은 없다.
- perf helper `send_mutex` 진단을 제외하면 379K~386K에 접근한 core-only 증거가 없다.
- 따라서 이번 round에서는 source 변경을 추가하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. source 변경 없이 측정만 수행했다.
- 보안 의미를 유지한 근거:
  - mtrie 비재귀화, WS/WSS pending-copy 제거, 포트 파싱, IPC unlink 순서, decoder/message/send guard,
    `maxmsgsize` 정책을 변경하지 않는다.
