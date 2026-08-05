# Round 51: one-way 64B 반복 결손 재확인

- 목표: 전체 64B 공통 항목의 평균/중앙값 개선 후보를 찾기 위해, core source
  변경 전에 one-way 64B 결손이 현재 checkout에서 반복되는지 재확인한다.
- 완료 기준:
  - `DEALER_DEALER,PUBSUB,SPOT` 64B targeted perf에서 문제 report 대비 10%
    이상 낮은 항목을 분리한다.
  - 반복 결손이 있으면 이미 실패한 후보를 제외하고 새 core hot path 후보를 하나만
    적용한다.
  - source 변경 후 `cmake --build core/build -j$(nproc)`, 관련 CTest, targeted perf를
    실행한다.
- 시작 시각: 2026-06-15 KST
- 시작 git status:
  - core/perf source diff: empty
  - perf log files under `doc/plan/perf/core/log` are untracked
- 기준 report:
  - baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - problem: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
  - latest 64B sweep:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_025722_round43_current_64b_lowload_sweep.txt`

## 기준 수치

- problem report one-way 64B:
  - `MULTI_DEALER_DEALER` tcp/tls/ws/wss:
    `3,045,747.2 / 3,162,931.4 / 3,156,838.0 / 3,265,432.2`
  - `MULTI_PUBSUB` tcp/tls/ws/wss:
    `2,628,104.8 / 2,446,707.8 / 2,220,372.2 / 2,679,903.2`
  - `MULTI_SPOT` tcp/tls:
    `3,896,078.6 / 3,739,003.6`
- round43 current 64B:
  - `MULTI_DEALER_DEALER` tcp/tls/ws/wss:
    `2,906,389.0 / 3,024,856.0 / 2,982,144.0 / 3,115,062.4`
  - `MULTI_PUBSUB` tcp/tls/ws/wss:
    `2,490,019.2 / 2,302,661.0 / 2,110,001.8 / 2,518,655.0`
  - `MULTI_SPOT` tcp/tls/ws/wss:
    `3,515,067.8 / 3,530,761.2 / 3,520,855.2 / 3,824,656.2`

## 가설

1. one-way 64B 결손은 현재 checkout에서도 일부 pattern/transport에서 10% 이상
   반복되며, 아직 남은 core 병목은 pipe dequeue/enqueue 또는 ASIO write batching에 있다.
2. 문제 report 대비 10% 결손은 현재 낮은 부하 반복에서 재현되지 않으며, 남은 큰
   baseline gap은 이미 확인한 perf-helper/ASIO 구조/측정 의미 변화가 더 크다.
3. 반복 결손이 특정 pattern에만 남으면 해당 pattern 전용 hot path를 본다. 단,
   이미 실패한 후보는 반복하지 않는다:
   SPOT ingress 직접 forward, per-message pump 제거, local fanout refresh guard,
   SPOT DONTWAIT admission, PUBSUB publish_part fast path, repeated-topic matching cache,
   distributor HWM precheck, LMSG pool.

선택한 첫 확인: source 변경 없이 targeted one-way 64B를 재측정한다.

## 읽은 코드와 이전 라운드

- `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`
- `core/src/runtime/sockets/pubsub/xpub.cpp`
- `core/src/runtime/sockets/internal/dist.cpp`
- `doc/plan/perf/core/log/2026-06-15-round-33-spot-fanout-repeat.ko.md`
- `doc/plan/perf/core/log/2026-06-15-round-34-pubsub-repeat.ko.md`
- `doc/plan/perf/core/log/2026-06-15-round-37-spot-ingress-dontwait-admission.ko.md`
- `doc/plan/perf/core/log/2026-06-15-round-42-publish-part-final-fastpath.ko.md`
- `doc/plan/perf/core/log/2026-06-15-round-46-dist-prechecked-hwm-write.ko.md`

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거: 이번 라운드는 source 변경 없이 반복 측정만 수행했다.
  WS/WSS pending message copy 제거, mtrie 비재귀화, 포트 파싱, IPC unlink 순서,
  decoder/message/send guard, maxmsgsize 정책을 건드리지 않았다.
- 추가로 실행한 회귀 테스트: source 변경 없음.

## 검증

- targeted perf:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --pattern DEALER_DEALER,PUBSUB,SPOT --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round51_oneway_64b_repeat`
  - runner runtime:
    `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_041202_round51_oneway_64b_repeat.txt`
  - completion: success 12, fail 0, status complete.
  - load_avg: `3.80 14.67 13.78`.

## 결과

| pattern | transport | round51 | problem | delta |
|---------|-----------|---------|---------|-------|
| `MULTI_DEALER_DEALER` | tcp | `2,908,213.0` | `3,045,747.2` | `-4.52%` |
| `MULTI_DEALER_DEALER` | tls | `3,006,259.2` | `3,162,931.4` | `-4.95%` |
| `MULTI_DEALER_DEALER` | ws | `2,961,377.2` | `3,156,838.0` | `-6.19%` |
| `MULTI_DEALER_DEALER` | wss | `3,113,236.0` | `3,265,432.2` | `-4.66%` |
| `MULTI_PUBSUB` | tcp | `2,563,680.8` | `2,628,104.8` | `-2.45%` |
| `MULTI_PUBSUB` | tls | `2,272,493.0` | `2,446,707.8` | `-7.12%` |
| `MULTI_PUBSUB` | ws | `2,105,585.2` | `2,220,372.2` | `-5.17%` |
| `MULTI_PUBSUB` | wss | `2,543,040.2` | `2,679,903.2` | `-5.11%` |
| `MULTI_SPOT` | tcp | `3,534,869.0` | `3,896,078.6` | `-9.27%` |
| `MULTI_SPOT` | tls | `4,226,129.2` | `3,739,003.6` | `+13.03%` |

`MULTI_SPOT ws/wss`는 problem report에 공통 기준값이 없어 delta 판정에서 제외했다.
측정값은 ws `3,393,428.6`, wss `3,367,600.8`이고 둘 다 성공했다.

## 판정

- 목표 달성 여부: 미달성.
- 이번 반복에서는 문제 report 대비 10% 이상 낮은 one-way 공통 항목이 없다.
- `MULTI_SPOT/tcp/64`가 `-9.27%`로 가장 낮지만, 10% 반복 결손 기준 바로 아래이며
  run별 편차도 `3.27M -> 3.53M -> 3.82M`으로 크다. 이 수치만으로 source 변경을
  넣지 않는다.
- 이미 실패/혼재 판정된 SPOT/PUBSUB 후보를 반복하지 않는다.
- 다음 후보는 one-way source 변경이 아니라 full multi 실패 0 또는 echo 계열에서
  반복 결손이 남는지 확인한 뒤 선정한다.
