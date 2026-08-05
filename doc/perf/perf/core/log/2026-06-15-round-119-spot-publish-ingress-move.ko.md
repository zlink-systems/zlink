# Round 119: SPOT publish ingress move

## 이번 라운드 목표

- SPOT one-way 64B publish ingress 경로에서 user parts를 data-plane queue entry로 옮기는 비용을 줄인다.
- 완료 기준:
  - 관련 SPOT/PUBSUB/core tests 통과.
  - `MULTI_SPOT` 64B targeted perf에서 실패 0개.
  - 인접 one-way set에서 하락 항목 없이 순효과가 있거나, 최소한 변경 효과가 없는 경우 source를 되돌린다.

## 기준 report

- historical baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- corrected full baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- problem report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- latest useful reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_104531_round65_final_spot_restore_all64_reduced_full.txt`

## 시작 상태

- `core/src`, `core/include`, `core/tests`: source diff 없음에서 시작.
- perf runner/client/server는 수정하지 않는다.
- `git status --short`에는 기존 perf log untracked 파일이 많다.

## 기준 수치

`perf_c_multi_linux_20260513_101034.txt` 대비 `perf_c_multi_linux_20260614_103936.txt`
공통 64B worst:

| Pattern | Transport | historical | problem | Delta |
|---------|-----------|-----------:|--------:|------:|
| MULTI_SPOT | tcp | 7379815.4 | 3896078.6 | -47.2% |
| MULTI_SPOT | tls | 6924687.4 | 3739003.6 | -46.0% |
| MULTI_STREAM | ws | 366639.2 | 243650.8 | -33.5% |
| MULTI_PUBSUB | tls | 3333680.8 | 2446707.8 | -26.6% |
| MULTI_PUBSUB | tcp | 3518022.8 | 2628104.8 | -25.3% |

## 가설

- 가설 1:
  `zlink_spot_publish()`는 성공 시 caller parts를 소비한다. 현재 `enqueue_publish_ingress()`는
  user parts를 data-plane entry로 `copy`한 뒤 성공 시 원본을 close한다. queue admission이 끝난 뒤
  `move`하면 64B publish hot path의 copy/refcount 비용을 줄일 수 있다.
- 가설 2:
  SPOT publish는 data-plane queue와 fanout이 지배적이어서 parts copy 제거만으로는 효과가 작을 수 있다.
  이 경우 변경을 남기지 않는다.
- 먼저 검증할 가설:
  가설 1. 실패/timeout/closed 경로에서는 원본 parts를 건드리지 않고, admission 이후 성공 경로에서만
  move하도록 구현한다.

## 읽은 코드

- `core/src/runtime/services/spot/pubsub/spot_subject_publish.cpp`
  - SPOT facade publish는 `spot_data_plane_forwarder_t::enqueue_publish_ingress()`로 들어간다.
- `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`
  - 기존 `enqueue_publish_ingress()`는 `copy_raw_parts_to_owned()` 후 queue에 넣고 성공 시
    `zlink_multipart_close()`로 원본을 소비한다.
  - queue admission 실패, timeout, shutdown 경로에서는 원본을 소비하지 않는다.
- `core/src/runtime/core/msg.cpp`
  - `msg_t::move()`는 destination으로 message를 옮기고 source를 empty initialized message로 되돌린다.

## 변경

- 변경 파일:
  - `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`
- 변경 내용:
  - admission 전에는 raw parts의 encoded byte만 계산한다.
  - queue room이 확정된 뒤 `move_raw_parts_to_owned()`로 parts ownership을 data-plane entry에 넘긴다.
  - 실패/timeout/closed 경로는 기존처럼 caller parts를 보존한다.
- perf 전용 변경이 아닌 이유:
  - public SPOT publish 성공 경로의 message ownership 이전 비용을 줄이는 core runtime 변경이다.
  - perf runner/client/server와 측정 조건은 바꾸지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 직접 해당 없음. SPOT publish queue ownership 이전만 변경한다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거, mtrie 비재귀화, 포트 파싱, IPC unlink,
    decoder/message/send guard, maxmsgsize 정책을 수정하지 않는다.
- 추가로 실행한 회귀 테스트:
  - 아래에 기록한다.

## 실행 결과

### Build

```bash
cmake --build core/build -j$(nproc)
```

- 결과: 성공.
- runtime: `core/build/lib/libzlink.so.6.0.4`.

### 관련 테스트

```bash
ctest --test-dir core/build --output-on-failure -R 'test_(helper_send_part_basic|helper_recv_part_basic|helper_ownership|helper_interleave|helper_more_bad_send|helper_request_sequence_failure|spot_pubsub_scenario|spot_poller|spot_runtime_activation|spot_dispatch_event|spot_router_channel_peer|transport_matrix|multi_socket_contract_regressions|pubsub|pubsub_filter_xpub|xpub_nodrop)$|unittest_spot_data_plane_|unittest_service_mode_policy|unittest_spot_subject_access'
```

- 결과: 20개 중 19개 통과.
- 실패:
  - `test_xpub_nodrop`: `blocking publish timeout: sent=3825 recv=3825`.
- focused 재실행:

```bash
ctest --test-dir core/build --output-on-failure -R '^test_xpub_nodrop$' --repeat until-pass:3
```

- 결과: 통과.
- 판단:
  - 변경 파일은 SPOT publish ingress라 XPUB NODROP 경로를 직접 건드리지 않는다.
  - 다만 채택 판단에서는 실패 이력을 보수적으로 기록한다.

### SPOT focused perf: 후보 적용 상태

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round119_spot_publish_ingress_move
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_180741_round119_spot_publish_ingress_move.txt`
- status: complete, fail 0.
- load_avg: `17.40 11.05 8.28`.

| Pattern | Transport | Throughput |
|---------|-----------|-----------:|
| MULTI_SPOT | tcp | 3666700.0 |
| MULTI_SPOT | tls | 6803819.0 |
| MULTI_SPOT | ws | 5439240.2 |
| MULTI_SPOT | wss | 6869106.6 |

### Adjacent one-way perf: 후보 적용 상태

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,PUBSUB,SPOT --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round119_oneway_adjacent_spot_publish_ingress_move
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_181617_round119_oneway_adjacent_spot_publish_ingress_move.txt`
- status: complete, fail 0.
- load_avg: `3.68 5.39 6.54`.

| Pattern | Transport | Throughput |
|---------|-----------|-----------:|
| MULTI_DEALER_DEALER | tcp | 2872586.2 |
| MULTI_DEALER_DEALER | tls | 3013900.4 |
| MULTI_DEALER_DEALER | ws | 2972244.8 |
| MULTI_DEALER_DEALER | wss | 3128680.2 |
| MULTI_PUBSUB | tcp | 2258718.6 |
| MULTI_PUBSUB | tls | 2143587.0 |
| MULTI_PUBSUB | ws | 1971533.4 |
| MULTI_PUBSUB | wss | 2433620.8 |
| MULTI_SPOT | tcp | 3635800.0 |
| MULTI_SPOT | tls | 6794347.6 |
| MULTI_SPOT | ws | 5566431.4 |
| MULTI_SPOT | wss | 6844541.6 |

### SPOT focused perf: 원복 A/B

후보 적용 상태가 transport별 tradeoff를 보였으므로 source를 원복한 뒤 같은 SPOT-only 조건으로 다시 측정했다.

```bash
cmake --build core/build --target libzlink -j$(nproc)
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round119_spot_publish_ingress_move_revert_ab
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_220808_round119_spot_publish_ingress_move_revert_ab.txt`
- status: complete, fail 0.
- load_avg: `2.31 2.46 1.71`.

| Transport | 후보 적용 | 원복 A/B | 후보 Delta |
|-----------|----------:|---------:|-----------:|
| tcp | 3666700.0 | 3982460.0 | -7.93% |
| tls | 6803819.0 | 5782939.4 | +17.65% |
| ws | 5439240.2 | 5983196.8 | -9.09% |
| wss | 6869106.6 | 6079319.8 | +12.99% |

## 최종 판단

- 미채택.
- `tls`, `wss`는 좋아졌지만 `tcp`, `ws`가 각각 약 `-7.9%`, `-9.1%` 하락했다.
- 이번 목표는 SPOT 일부 transport만이 아니라 전체 64B one-way와 하락 없는 개선이므로, 이 tradeoff는 남기지 않는다.
- source는 원복 상태로 둔다.
- perf runner/client/server는 수정하지 않았다.
- 보안 하드닝 의미는 변경하지 않았다.

## 다음 후보

- SPOT publish ownership move는 transport별 균형이 나빠져 배제한다.
- 다음 라운드는 PUBSUB `tcp/tls/ws` 회귀를 다시 current same-window 기준으로 재측정하고,
  mtrie/matching이 아니라 socket output 또는 PUB/SUB distribution 정책 중 하락 없는 후보가 있는지 본다.
