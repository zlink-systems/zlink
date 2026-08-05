# Round 74: 남은 회귀 항목 standalone 재확인

- goal:
  - round70 reduced full에서 May26 full 대비 크게 낮았던 항목이 실제 반복 회귀인지 standalone low-load로
    분리한다.
  - 완료 기준: targeted standalone perf 완료, May26 full/problem 대비 비교, 반복 회귀 후보와 제외 후보
    분리.
- 시작 시각: 2026-06-15 13:09:56 KST
- 기준 commit: `3e0e3956b`
- 시작 load_avg:
  - `/proc/loadavg`: `0.62 8.55 9.47`
- corrected baseline:
  - May26 full:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
  - May26 smoke:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- problem report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- current report:
  - round70 reduced full:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_123133_round70_current_reduced_full_refresh.txt`
- 시작 git 상태:
  - core source diff는 SPOT logical queue 및 global part-helper restore 계열만 남아 있다.
  - round71, round72 후보는 원복되어 source diff가 없다.

## 재확인 대상

- May26 full 대비 round70 worst:
  - `MULTI_SPOT/wss/64B`: `-14.70%`
  - `MULTI_PUBSUB/wss/64B`: `-9.23%`
  - `MULTI_SPOT_SENDSEND/tls/64B`: `-7.74%`
  - `MULTI_SPOT_SENDSEND/tcp/64B`: `-7.24%`
- 판단:
  - `PUBSUB/tls`는 round71 low-load에서도 반복 하락으로 확인했다.
  - 위 항목들은 full/reduced-full 순서와 load 영향인지 아직 분리되지 않았다.

## 가설

- 가설 1:
  - `SPOT/wss`와 `SPOT_SENDSEND tcp/tls` 하락은 reduced-full run-order/load 영향이다.
    standalone low-load 반복에서는 May26 full에 가까워질 수 있다.
- 가설 2:
  - standalone에서도 May26 full 대비 `-10%` 안팎이면 해당 항목은 반복 회귀 후보이며, 다음 코드 추적
    대상이 된다.
- 가설 3:
  - `PUBSUB/wss`가 standalone에서 회복되고 `PUBSUB/tls`만 반복 하락이면 다음 코드는 TLS-specific
    PUBSUB 경로를 더 좁혀야 한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - source 변경 없음.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message, mtrie 비재귀화, 포트 파싱, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않는다.
- 추가로 실행한 회귀 테스트:
  - source 변경 없음. 이번 round는 측정 분리만 수행한다.

## 실행

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern SPOT,PUBSUB,SPOT_SENDSEND \
  --transports wss,tcp,tls \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round74_remaining_regression_standalone_recheck
```

- runner runtime:
  - `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- runner meta load_avg:
  - `0.37 7.73 9.17`
- effective transports:
  - `tcp,tls,wss`
- report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_131025_round74_remaining_regression_standalone_recheck.txt`
- completion:
  - success: 9
  - unsupported: 0
  - skip: 0
  - fail: 0
  - status: complete

## May26 full 대비

| 항목 | May26 full | round74 | delta |
|------|------------|---------|-------|
| `MULTI_SPOT/tcp/64` | `3,962,360.0` | `3,917,180.0` | `-1.14%` |
| `MULTI_SPOT/tls/64` | `5,939,903.4` | `5,679,198.6` | `-4.39%` |
| `MULTI_SPOT/wss/64` | `6,776,300.6` | `6,938,274.2` | `+2.39%` |
| `MULTI_PUBSUB/tcp/64` | `2,661,635.6` | `2,493,177.8` | `-6.33%` |
| `MULTI_PUBSUB/tls/64` | `2,623,065.0` | `2,274,859.0` | `-13.27%` |
| `MULTI_PUBSUB/wss/64` | `2,760,571.0` | `2,504,005.6` | `-9.29%` |
| `MULTI_SPOT_SENDSEND/tcp/64` | `271,206.0` | `251,759.8` | `-7.17%` |
| `MULTI_SPOT_SENDSEND/tls/64` | `254,009.6` | `251,571.4` | `-0.96%` |
| `MULTI_SPOT_SENDSEND/wss/64` | `252,557.8` | `253,040.0` | `+0.19%` |

## problem report 대비

| 항목 | problem | round74 | delta |
|------|---------|---------|-------|
| `MULTI_SPOT/tcp/64` | `3,896,078.6` | `3,917,180.0` | `+0.54%` |
| `MULTI_SPOT/tls/64` | `3,739,003.6` | `5,679,198.6` | `+51.89%` |
| `MULTI_PUBSUB/tcp/64` | `2,628,104.8` | `2,493,177.8` | `-5.13%` |
| `MULTI_PUBSUB/tls/64` | `2,446,707.8` | `2,274,859.0` | `-7.02%` |
| `MULTI_PUBSUB/wss/64` | `2,679,903.2` | `2,504,005.6` | `-6.56%` |
| `MULTI_SPOT_SENDSEND/tcp/64` | `247,978.4` | `251,759.8` | `+1.52%` |
| `MULTI_SPOT_SENDSEND/tls/64` | `236,013.6` | `251,571.4` | `+6.59%` |

## 판정

- 반복 회귀에서 제외:
  - `MULTI_SPOT/wss/64`
    - May26 full 대비 `+2.39%`로 회복됐다.
  - `MULTI_SPOT_SENDSEND/tls/64`, `MULTI_SPOT_SENDSEND/wss/64`
    - May26 full 대비 동급이다.
  - `MULTI_SPOT/tcp,tls`
    - May26 full 대비 `-5%` 이내 또는 problem 대비 크게 개선된 상태다.
- 약한 하락:
  - `MULTI_SPOT_SENDSEND/tcp/64`: May26 full 대비 `-7.17%`.
  - `MULTI_PUBSUB/tcp/64`: May26 full 대비 `-6.33%`.
  - `MULTI_PUBSUB/wss/64`: May26 full 대비 `-9.29%`.
  - 위 항목은 부담이지만 `-10%` 반복 회귀 기준에는 못 미친다.
- 반복 회귀 후보:
  - `MULTI_PUBSUB/tls/64`: May26 full 대비 `-13.27%`.
  - round71 low-load에서도 `-13.62%`였으므로 반복성이 있다.
- 다음 코드 추적 대상:
  - `PUBSUB/tls`에 한정한다.
  - 다만 이미 실패한 후보를 반복하지 않는다:
    - TLS write completion 방식 변경.
    - TLS speculative write enable.
    - ASIO handler allocator 전체 확대.
    - mtrie match functor overload.
    - XSUB 단일 subscription cache.
    - PUBSUB empty-subscription active pipe 상태 추가.
