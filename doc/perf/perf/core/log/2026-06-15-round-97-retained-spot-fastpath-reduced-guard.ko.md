# Round 97: retained SPOT fast path reduced guard

## goal

round92에서 유지한 `SPOT_SENDSEND` 단일 FINAL fast path가 인접 64B set에서 하락을 만들지 않는지
확인한다. 완료 기준은 reduced guard에서 실패 0개와 `SPOT_SENDSEND` 개선 재현이다. 실패가 나오면
성능 개선보다 실패 분리를 우선한다.

## 기준

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- May26 smoke:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- round90 current:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_145417_round90_current_regression_recalibration.txt`

## git 상태

- core source diff:
  - `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
- retained change:
  - `zlink_spot_send_spot_part()`에서 단일 `ZLINK_PART_FINAL`이고 send sequence가 열려 있지 않으면
    `spot_send_spot_impl()`을 직접 호출한다.
- unrelated dirty files:
  - framework docs/node sample 변경들이 worktree에 섞여 있으나 이 round에서 건드리지 않았다.

## 병목 가설

1. `SPOT_SENDSEND` 서버 echo 경로는 단일 FINAL part를 staged sequence로 보내는 비용이 남아 있었고,
   round92 fast path가 이 비용을 줄인다.
2. `PUBSUB/tls`의 반복 하락은 retained SPOT 변경과 무관하며, ASIO/TLS 또는 PUB/SUB fanout 쪽에 남은
   별도 문제다.
3. `STREAM/tls` 실패는 단독 기능 실패가 아니라 reduced run에서 앞선 pattern 이후 발생하는 transition
   또는 자원 상태와 결합된 실패일 수 있다.

먼저 1번을 reduced guard로 확인하고, 실패가 나오면 3번을 standalone으로 분리한다.

## reduced guard

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern PUBSUB,SPOT_SENDSEND,SPOT_REQREP,STREAM \
  --transports tcp,tls,wss \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round97_retained_spot_fastpath_reduced_guard
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_153913_round97_retained_spot_fastpath_reduced_guard.txt`
- status: partial
- success: 10
- fail: 2
- start load_avg: `1.18 6.02 8.49`
- failure:
  - `MULTI_STREAM current tls 64B: non_zero_exit_2_size_64`

| case | round90 current | round97 | delta |
|---|---:|---:|---:|
| PUBSUB/tcp/64B | 2,548,363.4 | 2,395,925.0 | -5.98% |
| PUBSUB/tls/64B | 2,278,477.4 | 2,253,781.4 | -1.08% |
| PUBSUB/wss/64B | 2,494,252.0 | 2,486,570.4 | -0.31% |
| SPOT_SENDSEND/tcp/64B | 245,644.8 | 254,622.0 | +3.65% |
| SPOT_SENDSEND/tls/64B | 233,617.4 | 250,650.6 | +7.29% |
| SPOT_SENDSEND/wss/64B | 248,696.6 | 249,630.6 | +0.38% |
| SPOT_REQREP/tcp/64B | n/a | 252,201.8 | n/a |
| SPOT_REQREP/tls/64B | n/a | 230,525.2 | n/a |
| SPOT_REQREP/wss/64B | n/a | 210,494.8 | n/a |
| STREAM/tcp/64B | 320,253.2 | 307,472.2 | -3.99% |
| STREAM/tls/64B | 219,801.0 | fail | n/a |
| STREAM/wss/64B | 190,692.6 | fail | n/a |

## STREAM/tls standalone 분리

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tls \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round97_stream_tls_failure_repro
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_154648_round97_stream_tls_failure_repro.txt`
- status: complete
- success: 1
- fail: 0
- start load_avg: `3.03 3.57 6.23`
- result: `STREAM/tls/64B = 219,094.8 ops/s`

단독 STREAM/tls는 성공했고 May26 full `214,574.6`보다 높다. reduced run의 실패는 순수 STREAM/tls
기능 실패라기보다 앞선 pattern 실행 뒤 자원/transition 상태와 결합된 실패로 본다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. 이 round는 source 변경 없이 retained diff를 guard했다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending copy 제거, mtrie 비재귀화, 포트 파싱, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 건드리지 않았다.
- 추가로 실행한 회귀 테스트:
  - source 변경 없음. 직전 retained source는 round92 focused CTest에서 검증했다.

## 판정

- `SPOT_SENDSEND/tls`는 round90 대비 `+7.29%`로 개선이 재현되었고 May26 full 대비 `-1.32%`까지
  회복됐다.
- `SPOT_SENDSEND/tcp`도 round90 대비 `+3.65%`이나 5% 기준에는 못 미친다.
- `SPOT_SENDSEND/wss`는 보합이다.
- reduced guard에서 STREAM/tls/wss 실패가 있어 전체 목표는 완료가 아니다.
- `PUBSUB/tcp/tls`와 `STREAM/tcp`는 여전히 다음 개선 후보로 남는다.

## 다음

- 새 성능 패치를 넣기 전, reduced/full run에서 STREAM 실패가 반복되는 조건을 더 좁힌다.
- 성능 후보는 `PUBSUB/tls`와 공통 one-way hot path를 우선한다. retained SPOT 변경은 특정 echo 경로
  개선으로 보존하되 전체 64B 목표 달성으로 보지는 않는다.
