# Round 120: PUBSUB current 재측정과 dist triage

## 이번 라운드 목표

- 남은 one-way gap인 `MULTI_PUBSUB` 64B를 현재 source에서 다시 측정한다.
- PUB/SUB matching/fanout 경로에서 POSD-safe 후보가 있는지 코드 기준으로 확인한다.
- 완료 기준:
  - source 변경 전 현재값 report 확보.
  - 병목 가설 2개 이상 검토.
  - 하락 없는 최소 core 후보가 있으면 build/test/perf로 검증하고, 효과가 없으면 원복한다.

## 기준 report

- historical baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- corrected full baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- problem report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- latest useful reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_104531_round65_final_spot_restore_all64_reduced_full.txt`
- round119 adjacent:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_181617_round119_oneway_adjacent_spot_publish_ingress_move.txt`

## 시작 상태

- `core/src`, `core/include`, `core/tests`: source diff 없음.
- perf runner/client/server는 수정하지 않는다.
- 이전 round119 후보는 `tcp/ws` 하락으로 미채택했고 source는 원복했다.

## 가설

- 가설 1:
  PUBSUB 64B gap은 run-order/load 영향이 크다. PUBSUB-only current 측정에서는 May26 full과 가까워질 수 있다.
- 가설 2:
  XPUB `dist_t` fanout 또는 subscription matching 경로에 작은 메시지에서 반복되는 불필요한 상태 확인이 있다.
  단, empty-subscription single-pipe cache 후보는 ws 하락과 상태 추가 때문에 이미 배제됐다.
- 가설 3:
  PUBSUB output은 socket/pipe 공통 send path보다 XPUB distribution 이후 transport write path의 부하가 크다.
  dist/mtrie만 바꿔도 개선이 없으면 source를 남기지 않는다.
- 먼저 검증할 가설:
  가설 1. source 변경 전 `PUBSUB tcp,tls,ws,wss 64B`를 단독으로 다시 측정한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 아직 없음. source 변경 전이다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거, mtrie 비재귀화, 포트 파싱, IPC unlink,
    decoder/message/send guard, maxmsgsize 정책을 수정하지 않는다.
- 추가로 실행한 회귀 테스트:
  - source 후보가 생기면 기록한다.

## source 변경 전 PUBSUB current 측정

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round120_pubsub_current_same_window`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_221807_round120_pubsub_current_same_window.txt`
- status: complete
- load_avg: `1.94 3.63 2.90`
- 결과:

| pattern | transport | current kops |
|---------|-----------|--------------|
| MULTI_PUBSUB | tcp | 2538.460 |
| MULTI_PUBSUB | tls | 2425.938 |
| MULTI_PUBSUB | ws | 2140.000 |
| MULTI_PUBSUB | wss | 2668.004 |

### corrected May26 full 대비

- 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`

| transport | May26 full kops | round120 current kops | delta |
|-----------|-----------------|-----------------------|-------|
| tcp | 2661.636 | 2538.460 | -4.63% |
| tls | 2623.065 | 2425.938 | -7.51% |
| ws | 2201.277 | 2140.000 | -2.78% |
| wss | 2760.571 | 2668.004 | -3.35% |

### corrected May26 smoke 대비

- 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`

| transport | May26 smoke kops | round120 current kops | delta |
|-----------|------------------|-----------------------|-------|
| tcp | 2677.051 | 2538.460 | -5.18% |
| tls | 2537.614 | 2425.938 | -4.40% |
| ws | 2193.446 | 2140.000 | -2.44% |
| wss | 2529.036 | 2668.004 | +5.50% |

## dist/mtrie triage

- `core/src/runtime/sockets/internal/dist.cpp`
  - 이미 `_matching == 1 && _active == 1 && _eligible == 1` 단일 subscriber hot path가 있다.
  - VSM 메시지는 refcount 추가 없이 matching pipe에 직접 write한다.
  - HWM 확인은 `_matching_hwm_cache_valid` / `_matching_hwm_ready`로 matching 집합 단위 캐시가 있다.
- `core/src/runtime/sockets/pubsub/xpub.cpp`
  - 일반 PUB send는 첫 part에서 `_subscriptions.match(...)`로 matching pipe를 고르고,
    이후 `_dist.check_hwm()`와 `_dist.send_to_matching(...)`로 내려간다.
  - empty subscription steady state에서는 `mtrie.match()`가 root의 pipe set만 방문한 뒤 끝난다.
- `core/src/runtime/utils/generic_mtrie_impl.hpp`
  - `rm(...)` 삭제 경로는 원격 입력이 재귀 깊이를 만들지 않도록 비재귀 stack traversal을 쓴다.
  - 이 구조는 보안 하드닝 결과이므로 성능만 보고 재귀화하거나 별도 얕은 cache를 추가하지 않는다.

## 판단

- PUBSUB-only current는 corrected May26 full 기준으로 모두 -10% 안쪽이다.
  따라서 round119 adjacent에서 보인 낮은 PUBSUB 값은 PUBSUB source 경로 단독 회귀라기보다
  혼합 실행 순서와 부하 영향으로 보는 편이 현재 증거에 맞다.
- dist/mtrie에는 이미 단일 subscriber, VSM, HWM 캐시가 들어가 있어 작은 추가 cache 후보의 기대값이 낮다.
- empty subscription cache처럼 상태를 더 얹는 후보는 지난 검증에서 `ws` 하락과 복잡도 증가가 확인됐으므로
  POSD 기준의 깊은 모듈/정보 은닉/오류 제거 측면에서 채택하지 않는다.
- 이번 라운드에서는 source 변경을 남기지 않는다.

## 최종 상태

- source 변경: 없음.
- perf runner/client/server 변경: 없음.
- 보안 하드닝 보존:
  - mtrie 비재귀 삭제 경로 유지.
  - WS/WSS pending message 전체 사본 제거, decoder/message/send guard, maxmsgsize 정책, IPC unlink,
    포트 파싱 변경을 건드리지 않음.
- 다음 후보:
  - STREAM 64B를 May26 기준으로 다시 current 측정한다.
  - 특히 사용자가 지적한 `tcp 64B` 목표와 May26 corrected baseline을 기준으로 transport별 하락 여부를 먼저 분리한다.
