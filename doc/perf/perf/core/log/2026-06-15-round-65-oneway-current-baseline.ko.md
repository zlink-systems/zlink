# Round 65: one-way 64B current baseline

- goal: `MULTI_SPOT`, `MULTI_PUBSUB`, `MULTI_DEALER_DEALER` 64B one-way hot path에서
  현재 재현 가능한 회귀 축을 다시 고른다.
- 완료 기준:
  - targeted one-way 64B set에서 문제 report 대비 중앙값 `+10%` 이상 또는 round51/65 clean
    대비 반복 `+10%` 이상 개선.
  - `cmake --build core/build -j$(nproc)` 통과.
  - 관련 core targeted tests 통과.
  - perf runner runtime이 `core/build` 아래임을 확인.
- 시작 시각: 2026-06-15 KST
- 기준 commit: `72d893595`
- 시작 git status:
  - `core/src`, `core/include`, `core/tests`, `bindings/c/perf` source diff 없음.
  - 기존 dotnet 문서 변경과 untracked perf log는 이 라운드에서 건드리지 않는다.
- 기준 report:
  - historical baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - corrected smoke baseline:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - corrected full baseline:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
  - problem: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 대상 pattern/transport/size:
  - `MULTI_SPOT`, `MULTI_PUBSUB`, `MULTI_DEALER_DEALER`
  - `tcp,tls,ws,wss`
  - `64B`

## 기준 수치

- 기준 정정:
  - 2026-05-13 baseline은 테스트 기준이 현재와 다를 가능성이 있어 historical 참고로 낮춘다.
  - 2026-05-26 full refresh 파일을 corrected primary baseline으로 사용한다.
  - 2026-05-26 smoke 파일은 보조 확인으로 사용한다.
- 판정 기준:
  - corrected baseline 대비 `-10%` 이내는 동급 또는 환경 변동 범위로 본다.
  - `-10%`에서 `-20%`는 관찰/재측정 대상으로 본다.
  - `-20%` 초과 하락은 source 후보를 찾는 회귀 후보로 본다.
  - source 변경 유지 기준은 기존 원칙대로 반복 `+10%` 수준의 명확한 개선이다.
- 2026-05-13 historical baseline/problem 공통 64B 항목: 26개.
- problem vs 2026-05-13 historical baseline:
  - 전체 평균: `-15.62%`
  - 전체 중앙값: `-14.86%`
  - one-way 평균: `-27.36%`
  - one-way 중앙값: `-22.86%`
- one-way worst:
  - `MULTI_SPOT/tcp/64`: `7,379,815.4 -> 3,896,078.6`, `-47.21%`
  - `MULTI_SPOT/tls/64`: `6,924,687.4 -> 3,739,003.6`, `-46.00%`
  - `MULTI_PUBSUB/tls/64`: `3,333,680.8 -> 2,446,707.8`, `-26.61%`
  - `MULTI_PUBSUB/tcp/64`: `3,518,022.8 -> 2,628,104.8`, `-25.30%`
  - `MULTI_DEALER_DEALER/tcp/64`: `3,965,901.8 -> 3,045,747.2`, `-23.20%`

## 가설

- 가설 1: 현재도 SPOT tcp/tls 64B가 가장 큰 반복 gap이고, SPOT data-plane publish drain 또는
  fanout path에 아직 유지 가능한 core hot path 개선점이 남아 있다.
- 가설 2: SPOT 단독 gap은 run-order/load noise가 크고, 더 안정적인 공통 축은
  `MULTI_DEALER_DEALER`/`MULTI_PUBSUB`의 pipe enqueue/dequeue 또는 PUB/SUB matching 쪽이다.
- 가설 3: mailbox/wakeup 단순 중복 제거는 round64에서 배제됐으므로, 이번 라운드에서 같은 후보를
  반복하지 않는다.
- 선택한 가설:
  - 먼저 clean targeted one-way 64B set을 재측정한다.
  - 같은 시간대 현재값이 corrected full baseline과 동급이거나 흔들리면 source 변경을 하지 않고 다음 후보를 고른다.
  - 반복 gap이 남는 pattern만 코드 경로를 좁힌다.

## Clean targeted one-way 재측정

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,PUBSUB,SPOT --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_oneway64_clean_current`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_072606_round65_oneway64_clean_current.txt`
- completion:
  `success=12`, `fail=0`, `status=complete`
- load_avg:
  `0.28 2.24 4.02`

## Corrected full baseline 비교

- corrected full baseline meta:
  - commit: `1b60c0159`
  - timestamp: `2026-05-26T23:30:10+09:00`
  - load_avg: `2.16 2.30 2.56`
  - runs: `1`
  - status: `complete`
- round65 current meta:
  - commit: `72d893595`
  - timestamp: `2026-06-15T07:26:06+09:00`
  - load_avg: `0.28 2.24 4.02`
  - runs: `5`
  - status: `complete`

| 판정 | 항목 | corrected full | round65 current | 차이 |
|------|------|----------------|-----------------|------|
| OK | `MULTI_DEALER_DEALER/tcp/64` | `2,980,045.0` | `2,891,215.2` | `-2.98%` |
| OK | `MULTI_DEALER_DEALER/tls/64` | `3,063,300.2` | `3,010,908.6` | `-1.71%` |
| OK | `MULTI_DEALER_DEALER/ws/64` | `3,080,453.4` | `2,949,556.8` | `-4.25%` |
| OK | `MULTI_DEALER_DEALER/wss/64` | `3,217,510.6` | `3,125,131.2` | `-2.87%` |
| OBSERVE | `MULTI_PUBSUB/tcp/64` | `2,661,635.6` | `2,307,819.4` | `-13.29%` |
| OBSERVE | `MULTI_PUBSUB/tls/64` | `2,623,065.0` | `2,295,643.4` | `-12.48%` |
| OK | `MULTI_PUBSUB/ws/64` | `2,201,277.0` | `2,126,551.8` | `-3.39%` |
| OK | `MULTI_PUBSUB/wss/64` | `2,760,571.0` | `2,528,522.0` | `-8.41%` |
| OK | `MULTI_SPOT/tcp/64` | `3,962,360.0` | `3,765,960.0` | `-4.96%` |
| REGRESSION-CANDIDATE | `MULTI_SPOT/tls/64` | `5,939,903.4` | `3,478,277.0` | `-41.44%` |
| REGRESSION-CANDIDATE | `MULTI_SPOT/ws/64` | `5,788,890.8` | `4,053,863.0` | `-29.97%` |
| REGRESSION-CANDIDATE | `MULTI_SPOT/wss/64` | `6,776,300.6` | `3,466,985.6` | `-48.84%` |

## 1차 판정

- `MULTI_DEALER_DEALER`는 corrected full baseline 대비 모두 `-5%` 이내라 동급으로 본다.
- `MULTI_SPOT/tcp`도 `-4.96%`로 동급 경계다.
- source 후보 우선순위:
  1. `MULTI_SPOT/tls,ws,wss`가 standalone에서도 큰 차이를 유지하는지 확인한다.
  2. `MULTI_PUBSUB/tcp,tls`는 `-10%~-20%` 관찰 구간이므로 SPOT 원인 추적 뒤에 재측정한다.
- 같은 round 안에서 2026-05-13 기준만으로 source 변경을 선택하지 않는다.

## SPOT non-tcp standalone 재측정

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_spot_non_tcp64_standalone`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_074120_round65_spot_non_tcp64_standalone.txt`
- completion:
  `success=3`, `fail=0`, `status=complete`
- load_avg:
  `0.89 2.23 2.99`

| 판정 | 항목 | corrected full | standalone current | 차이 |
|------|------|----------------|--------------------|------|
| REGRESSION-CANDIDATE | `MULTI_SPOT/tls/64` | `5,939,903.4` | `3,369,314.0` | `-43.28%` |
| REGRESSION-CANDIDATE | `MULTI_SPOT/ws/64` | `5,788,890.8` | `4,052,109.2` | `-30.00%` |
| REGRESSION-CANDIDATE | `MULTI_SPOT/wss/64` | `6,776,300.6` | `3,474,999.2` | `-48.72%` |

- standalone에서도 SPOT non-tcp gap이 유지된다.
- 이 구간은 완화한 기준에서도 회귀 후보이므로 source 후보를 찾는다.

## A/B: May 26 방식의 vector publish 복원

- 후보:
  - `548bdbc1b perf(core): inline small spot publish frame buffer` 이후 SPOT publish 복사 경로가
    바뀌었으므로, `spot_publish_msg_parts`를 May 26 방식에 가까운 vector copy 방식으로 되돌려
    측정했다.
- source 상태:
  - A/B 측정 뒤 source 변경은 되돌렸다.
- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_spot_non_tcp64_ab_vector_publish`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_075450_round65_spot_non_tcp64_ab_vector_publish.txt`
- completion:
  `success=3`, `fail=0`, `status=complete`
- load_avg:
  `20.82 8.45 4.62`

| 항목 | standalone current | A/B vector publish | 차이 |
|------|--------------------|--------------------|------|
| `MULTI_SPOT/tls/64` | `3,369,314.0` | `3,429,965.8` | `+1.80%` |
| `MULTI_SPOT/ws/64` | `4,052,109.2` | `4,026,390.4` | `-0.63%` |
| `MULTI_SPOT/wss/64` | `3,474,999.2` | `3,461,835.6` | `-0.38%` |

- 판정:
  - 시작 load average가 높아 절대값 판정에는 주의가 필요하다.
  - 그러나 세 transport 모두 기존 standalone과 동급이라 이 변경은 회귀 원인으로 보지 않는다.
  - 다음 후보는 SPOT parts 복사보다 non-tcp transport/socket 공통 경로에서 찾는다.

## 변경: submit-retry fault hook 원자 hot path 제거

- 후보:
  - `core/build`는 `ZLINK_BUILD_TESTS=1`로 libzlink를 빌드한다.
  - `socket_base_t::send_direct_with_retry`의 submit-retry fault hook은 테스트 전용인데,
    비활성 상태에서도 모든 send에서 전역 atomic load/CAS 경로를 확인했다.
  - fault 설정은 현재 테스트가 send 호출 스레드에서 직접 수행하므로 thread-local 카운터로 바꾸면
    테스트 의미를 유지하면서 steady-state send hot path의 원자 접근을 없앨 수 있다.
- source:
  - `core/src/runtime/sockets/common/socket_submit_retry_fault_injection.hpp`
  - `core/src/runtime/sockets/common/socket_submit_retry_fault_injection.cpp`
- 검증:
  - `cmake --build core/build -j$(nproc)` 통과.
  - `ctest --test-dir core/build --output-on-failure -R 'test_(reconnect_options|spot_pubsub_scenario|spot_poller|spot_runtime_activation|spot_dispatch_event|spot_router_channel_peer|transport_matrix|multi_socket_contract_regressions|pubsub|pubsub_filter_xpub|xpub_nodrop)$|unittest_spot_data_plane_'`
    통과: 13/13.

### SPOT non-tcp 반복 측정

| 항목 | standalone current | thread-local run 1 | thread-local run 2 | 판정 |
|------|--------------------|--------------------|--------------------|------|
| `MULTI_SPOT/tls/64` | `3,369,314.0` | `4,177,690.8` | `4,075,637.4` | 개선 재현 |
| `MULTI_SPOT/ws/64` | `4,052,109.2` | `3,539,058.6` | `3,487,511.4` | mixed |
| `MULTI_SPOT/wss/64` | `3,474,999.2` | `3,970,109.0` | `4,147,679.6` | 개선 재현 |

- run 1 report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_080621_round65_spot_non_tcp64_threadlocal_submit_fault.txt`
- run 2 report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_081248_round65_spot_non_tcp64_threadlocal_submit_fault_repeat.txt`
- ws 단독 확인:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports ws --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_spot_ws64_threadlocal_submit_fault_standalone`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_081915_round65_spot_ws64_threadlocal_submit_fault_standalone.txt`
  - result:
    `MULTI_SPOT/ws/64 = 4,039,739.4`
- 판정:
  - tls/wss는 repeated `+10%` 이상 개선을 보였다.
  - ws는 같은 multi run 안에서는 낮았지만 ws 단독에서는 기존 standalone과 동급이다.
  - 변경은 유지 후보로 두되, adjacent one-way set에서 `DEALER_DEALER`, `PUBSUB`, `SPOT` 전체
    부작용을 확인해야 한다.

### Adjacent one-way set 확인

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,PUBSUB,SPOT --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_oneway64_threadlocal_submit_fault`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_082150_round65_oneway64_threadlocal_submit_fault.txt`
- load_avg:
  `2.06 3.05 3.91`
- completion:
  `success=12`, `fail=0`, `status=complete`

| 항목 | corrected full | thread-local adjacent | 차이 |
|------|----------------|-----------------------|------|
| `MULTI_DEALER_DEALER/tcp/64` | `2,980,045.0` | `2,901,646.4` | `-2.63%` |
| `MULTI_DEALER_DEALER/tls/64` | `3,063,300.2` | `3,015,736.8` | `-1.55%` |
| `MULTI_DEALER_DEALER/ws/64` | `3,080,453.4` | `2,982,782.0` | `-3.17%` |
| `MULTI_DEALER_DEALER/wss/64` | `3,217,510.6` | `3,168,770.2` | `-1.51%` |
| `MULTI_PUBSUB/tcp/64` | `2,661,635.6` | `2,589,467.2` | `-2.71%` |
| `MULTI_PUBSUB/tls/64` | `2,623,065.0` | `2,259,725.8` | `-13.85%` |
| `MULTI_PUBSUB/ws/64` | `2,201,277.0` | `2,184,092.2` | `-0.78%` |
| `MULTI_PUBSUB/wss/64` | `2,760,571.0` | `2,528,435.6` | `-8.41%` |
| `MULTI_SPOT/tcp/64` | `3,962,360.0` | `3,832,500.0` | `-3.28%` |
| `MULTI_SPOT/tls/64` | `5,939,903.4` | `3,437,321.0` | `-42.13%` |
| `MULTI_SPOT/ws/64` | `5,788,890.8` | `4,024,912.2` | `-30.47%` |
| `MULTI_SPOT/wss/64` | `6,776,300.6` | `3,408,124.8` | `-49.71%` |

- 판정:
  - adjacent set에서는 `MULTI_SPOT/tls,wss` standalone 개선이 재현되지 않았다.
  - source 변경 유지 기준인 반복 `+10%` 개선을 충족하지 못한다.
  - 이 후보는 배제하고 source 변경을 되돌린다.

## 기준 완화 반영

- 사용자 확인 기준:
  - 단일 후보 또는 국소 최적화는 반복 측정에서 `+5%` 이상이면 개선 후보로 본다.
  - `-5% ~ +5%`는 노이즈 또는 동급으로 본다.
  - `-10%` 이상 반복 하락은 회귀 의심으로 본다.
  - 최종 라운드 성공 기준은 별도로 전체 64B 평균/중앙값과 큰 회귀 해소 여부를 본다.
- 작은 개선 후보 처리:
  - `1~2%` 후보도 같은 hot path에서 누적되면 의미가 있을 수 있다.
  - 단, 개별 `1~2%` 변경을 각각 확정해 남기면 측정 노이즈와 복잡도 증가가 섞일 수 있다.
  - 앞으로 작은 후보는 같은 경로 안에서 묶음으로 적용해 측정하고, 묶음 전체가 `+5%` 이상이면
    유지 후보로 본다. 묶음 전체가 `+5%` 미만이면 일괄 제거한다.
  - 단, 묶음 전체가 `+5%` 미만이어도 대상 묶음 전체가 플러스이고 개별 하락 항목이 없거나 모두
    노이즈 범위이면 유지 후보로 재검토한다. 반대로 한 항목이라도 실질 하락을 만들면 배제한다.
- 2026-05-13 baseline은 historical 참고로 낮추고, 2026-05-26 full/smoke baseline을 corrected
  baseline으로 우선 비교한다.

## 유지한 변경: SPOT logical queue 및 part-helper 복구

- source:
  - `core/src/runtime/services/spot/runtime/spot_handle.hpp`
  - `core/src/runtime/services/spot/node/spot_node_pubsub_fanout.cpp`
  - `core/src/api/spot/core/service_spot_api.cpp`
  - `core/src/runtime/services/spot/pubsub/spot_subject_query.cpp`
  - `core/src/api/socket/part_helper_api.cpp`
  - `core/src/runtime/services/spot/node/spot_node.cpp`
  - `core/src/runtime/services/spot/node/spot_node.hpp`
  - `core/src/runtime/services/spot/pubsub/spot_pub.cpp`
  - `core/src/runtime/services/spot/pubsub/spot_pub.hpp`
  - `core/src/runtime/services/spot/pubsub/spot_sub.cpp`
  - `core/src/runtime/services/spot/pubsub/spot_sub.hpp`
- 내용:
  - SPOT logical pubsub queue payload는 `std::vector<std::string>` 기반으로 복구했다.
  - SPOT 전용 part-helper state는 제거하고 기존 global part-helper state 경로를 사용하게 했다.
- 근거:
  - `7c168fdd4 perf(core): reduce spot multipart hot-path overhead` 도입 뒤 SPOT non-tcp가
    크게 하락했다.
  - temp A/B에서 SPOT/part-helper 관련 파일만 parent 방식으로 복구하면 `MULTI_SPOT/ws/64`가
    `~3.30M -> ~6.87M` 수준으로 회복했다.
  - main 적용 후 SPOT ws probe도 `6.82M` 수준으로 회복했다.
- 검증:
  - `cmake --build core/build -j$(nproc)` 통과.
  - 관련 CTest 18/18 통과:
    `test_helper_send_part_basic`, `test_helper_recv_part_basic`, `test_helper_ownership`,
    `test_helper_interleave`, `test_helper_more_bad_send`, `test_helper_request_sequence_failure`,
    `test_spot_pubsub_scenario`, `test_spot_poller`, `test_spot_runtime_activation`,
    `test_spot_dispatch_event`, `test_spot_router_channel_peer`, `test_transport_matrix`,
    `test_multi_socket_contract_regressions`, `test_pubsub`, `test_pubsub_filter_xpub`,
    `test_xpub_nodrop`, `unittest_spot_data_plane_*`.

## PUB/SUB empty-subscription fast path 후보: 배제

- 후보:
  - 일반 PUB/SUB steady state에서 빈 토픽 구독 파이프가 1개뿐이면 XPUB send 경로의 mtrie match를
    건너뛰는 fast path를 시험했다.
- 보존 조건:
  - `ZLINK_INTERNAL_OPT_XPUB_NODROP`의 HWM 의미가 깨지지 않도록 실제 pipe HWM을 확인하게
    조정했다.
- 검증:
  - `cmake --build core/build -j$(nproc)` 통과.
  - `ctest --test-dir core/build --output-on-failure -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|multi_socket_contract_regressions|backpressure_matrix|backpressure_oneway_matrix)$|unittest_mtrie'`
    통과: 5/5.
- perf command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tcp,tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_pubsub_empty_sub_fastpath_tcp_tls`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_103100_round65_pubsub_empty_sub_fastpath_tcp_tls.txt`
- 결과:

| 항목 | 직전 repeat | 후보 | 차이 | May26 full 대비 |
|------|-------------|------|------|----------------|
| `MULTI_PUBSUB/tcp/64` | `2,388,952.6` | `2,480,128.8` | `+3.82%` | `-6.82%` |
| `MULTI_PUBSUB/tls/64` | `2,244,905.8` | `2,297,908.4` | `+2.36%` | `-12.39%` |

- 판정:
  - 최초 tcp/tls 측정만 보면 `+5%` 채택 기준에는 못 미쳤지만 방향은 플러스였다.
  - 이후 사용자 기준을 반영해 "묶음 전체 플러스이고 하락 항목이 없으면 재검토" 대상으로 다시 올렸다.
  - 4 transport 재측정 결과는 아래 "PUB/SUB empty-subscription fast path 재검토"에 기록한다.
  - perf runner/client/server 변경 없음.

## SPOT_SENDSEND tcp/tls standalone 확인

- 목적:
  - reduced full에서 `MULTI_SPOT_SENDSEND/tcp,tls`가 크게 낮아 보였으므로 standalone 반복으로
    source 후보인지 확인했다.
- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT_SENDSEND --transports tcp,tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_spot_sendsend_tcp_tls_after_pubsub_candidate_revert`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_103459_round65_spot_sendsend_tcp_tls_after_pubsub_candidate_revert.txt`
- 결과:

| 항목 | May26 full | standalone current | 차이 |
|------|------------|--------------------|------|
| `MULTI_SPOT_SENDSEND/tcp/64` | `271,206.0` | `251,806.8` | `-7.15%` |
| `MULTI_SPOT_SENDSEND/tls/64` | `254,009.6` | `249,517.6` | `-1.77%` |

- 판정:
  - standalone 기준으로 `-10%` 반복 회귀가 아니다.
  - reduced full의 `191k~195k` 값은 run-order/load 영향 후보로 보고, 지금은 source 변경 후보에서
    내린다.
  - SPOT_SENDSEND 경로는 같은 프로세스 local target이어도 data-plane routed send queue를 거쳐
    순서를 맞춘다. 이 경로를 바로 local delivery로 우회하면 core 의미가 바뀔 수 있어 perf 개선
    후보로 적용하지 않았다.

## SPOT routed-send encoded-bytes cache 후보: 배제

- 후보:
  - `enqueue_runtime_routed_send`에서 이미 계산한 encoded byte size를
    `routed_send_entry_t`에 저장하고, `drain_runtime_routed_send_queue`와 retry requeue에서
    `routed_parts_encoded_bytes` 재계산을 피하는 작은 hot-path 후보를 시험했다.
- 의미:
  - HWM byte accounting 값을 재사용하는 변경이라 queue 의미나 backpressure 정책은 바꾸지 않는다.
- 검증:
  - `cmake --build core/build -j$(nproc)` 통과.
  - `ctest --test-dir core/build --output-on-failure -R 'test_(spot_poller|spot_runtime_activation|spot_dispatch_event|spot_router_channel_peer|spot_pubsub_scenario|multi_socket_contract_regressions|transport_matrix)$|unittest_spot_data_plane_'`
    통과: 9/9.
- perf command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT_SENDSEND --transports tcp,tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_spot_sendsend_encoded_bytes_cache_tcp_tls`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_104210_round65_spot_sendsend_encoded_bytes_cache_tcp_tls.txt`
- 결과:

| 항목 | 직전 standalone | 후보 | 차이 |
|------|-----------------|------|------|
| `MULTI_SPOT_SENDSEND/tcp/64` | `251,806.8` | `250,156.4` | `-0.66%` |
| `MULTI_SPOT_SENDSEND/tls/64` | `249,517.6` | `248,579.2` | `-0.38%` |

- 판정:
  - 작은 후보 묶음 기준으로도 방향이 없다.
  - source 변경은 되돌렸다.
  - perf runner/client/server 변경 없음.

## 최종 현재 상태 64B 축소 full 재측정

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round65_final_spot_restore_all64_reduced_full`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_104531_round65_final_spot_restore_all64_reduced_full.txt`
- completion:
  `success=32`, `fail=0`, `status=complete`
- problem report 대비:
  - common 64B: 26개
  - 전체 평균: `+5.13%`
  - 전체 중앙값: `+1.53%`
  - one-way 평균: `+4.82%`
  - one-way 중앙값: `-4.78%`
  - echo 평균: `+5.33%`
  - echo 중앙값: `+5.24%`
- May26 full 대비:
  - common 64B: 32개
  - 전체 평균: `-0.08%`
  - 전체 중앙값: `-0.26%`
  - one-way 평균: `-3.34%`
  - one-way 중앙값: `-2.69%`
  - echo 평균: `+1.88%`
  - echo 중앙값: `+1.40%`
- 주요 회복:
  - `MULTI_SPOT/tls/64`: problem 대비 `+85.83%`, May26 full 대비 `+16.98%`
  - `MULTI_SPOT/ws/64`: May26 full 대비 `-2.21%`
  - `MULTI_SPOT/wss/64`: May26 full 대비 `-15.08%`, 하지만 problem 대비 크게 회복
  - `MULTI_STREAM/tcp/64`: problem 대비 `+11.13%`, May26 full 대비 `+9.03%`
- 남은 gap:
  - problem 대비 worst는 `MULTI_PUBSUB/tcp,tls,wss`와 `MULTI_DEALER_DEALER` 일부다.
  - May26 full 대비 worst는 `MULTI_SPOT/wss`, `MULTI_PUBSUB/tls,wss,tcp`,
    `MULTI_SPOT_SENDSEND/tcp`이다.

## PUB/SUB empty-subscription fast path 재검토: 배제 유지

- 재검토 이유:
  - 최초 tcp/tls 측정은 `+3.82%`, `+2.36%`로 작지만 플러스였다.
  - 사용자 기준에 따라 "묶음 전체 플러스이고 하락 항목이 없으면 채택 가능"으로 다시 봤다.
- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_pubsub_empty_sub_fastpath_all_transport_recheck`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_110607_round65_pubsub_empty_sub_fastpath_all_transport_recheck.txt`
- completion:
  `success=4`, `fail=0`, `status=complete`
- 기준:
  - 직전 full 현재값:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_104531_round65_final_spot_restore_all64_reduced_full.txt`

| 항목 | 직전 full 현재 | 후보 재검토 | 차이 |
|------|----------------|-------------|------|
| `MULTI_PUBSUB/tcp/64` | `2,468,142.8` | `2,418,304.6` | `-2.02%` |
| `MULTI_PUBSUB/tls/64` | `2,305,984.6` | `2,283,721.6` | `-0.97%` |
| `MULTI_PUBSUB/ws/64` | `2,203,047.2` | `2,011,098.0` | `-8.71%` |
| `MULTI_PUBSUB/wss/64` | `2,530,473.6` | `2,511,240.6` | `-0.76%` |

- 판정:
  - 4 transport 묶음 기준으로 순효과가 플러스가 아니고, `ws`가 실질 하락했다.
  - "하락 항목 없이 플러스면 채택" 기준에도 실패한다.
  - source 변경은 다시 되돌렸다.
  - perf runner/client/server 변경 없음.

### 동일 시간대 no-candidate 재측정

- 목적:
  - full report와 단일 PUBSUB report의 부하 차이를 줄이기 위해 후보를 되돌린 직후 같은 형태로 다시 측정했다.
  - 작은 플러스 후보는 유지할 수 있지만, transport 중 하나라도 실질 하락하면 POSD 관점에서 상태 추가를 정당화하기 어렵다.
- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_pubsub_after_empty_sub_revert_same_window`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_111200_round65_pubsub_after_empty_sub_revert_same_window.txt`
- completion:
  `success=4`, `fail=0`, `status=complete`

| 항목 | no-candidate same-window | 후보 재검토 | 차이 |
|------|--------------------------|-------------|------|
| `MULTI_PUBSUB/tcp/64` | `2,333,041.0` | `2,418,304.6` | `+3.66%` |
| `MULTI_PUBSUB/tls/64` | `2,256,020.8` | `2,283,721.6` | `+1.23%` |
| `MULTI_PUBSUB/ws/64` | `2,121,295.6` | `2,011,098.0` | `-5.19%` |
| `MULTI_PUBSUB/wss/64` | `2,483,240.4` | `2,511,240.6` | `+1.13%` |

- 최종 판정:
  - 동일 시간대 비교에서는 tcp/tls/wss가 작게 플러스지만 `ws`가 `-5.19%` 하락했다.
  - 후보는 `_empty_subscription_pipes`와 single active pipe 상태를 추가하므로, 작은 평균 개선만으로는 복잡도 증가를 정당화하지 못한다.
  - 따라서 이 후보는 계속 배제하고, 하락 없는 더 단순한 공통 hot path 후보를 찾는다.

## submit-retry fault hook thread-local 후보 재검토: 배제

- 재검토 이유:
  - 이전 라운드에서는 SPOT 회귀 원인이 섞인 상태에서 배제했다.
  - SPOT logical queue와 part-helper 복구 뒤, 테스트 빌드에서만 남는 submit-retry fault hook의 전역 atomic 접근을 다시 확인했다.
- POSD 판단:
  - 릴리스 빌드에서는 `consume()`이 이미 `inline false`라 public runtime 비용이 없다.
  - `core/build` perf 기준에는 `ZLINK_BUILD_TESTS=1` 런타임을 쓰므로 측정상 후보가 될 수는 있다.
  - 하지만 테스트 전용 fault 주입 의미를 전역에서 thread-local로 바꾸는 변경은 작은 구현 변경이라도 의미를 좁힌다.
  - 따라서 adjacent set에서 하락 없이 플러스가 아니면 유지하지 않는다.
- 후보 변경:
  - `core/src/runtime/sockets/common/socket_submit_retry_fault_injection.cpp`
  - `std::atomic<int>` 전역 카운터를 `thread_local int` 카운터로 임시 변경했다.
- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- CTest:
  - command:
    `ctest --test-dir core/build --output-on-failure -R 'test_(reconnect_options|zmp_request_reply|spot_pubsub_scenario|spot_poller|spot_runtime_activation|spot_dispatch_event|spot_router_channel_peer|transport_matrix|multi_socket_contract_regressions|pubsub|pubsub_filter_xpub|xpub_nodrop)$|unittest_spot_data_plane_'`
  - result:
    14/14 통과.
- 후보 perf:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,PUBSUB,SPOT --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_threadlocal_submit_fault_after_spot_restore_recheck`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_112001_round65_threadlocal_submit_fault_after_spot_restore_recheck.txt`
  - runtime:
    `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - completion:
    `success=12`, `fail=0`, `status=complete`
- 원복 same-window:
  - 원복 뒤 `cmake --build core/build -j$(nproc)` 통과.
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,PUBSUB,SPOT --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_threadlocal_submit_fault_revert_same_window`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_113508_round65_threadlocal_submit_fault_revert_same_window.txt`
  - completion:
    `success=12`, `fail=0`, `status=complete`

| 항목 | 후보 | 원복 same-window | 차이 |
|------|------|------------------|------|
| `MULTI_DEALER_DEALER/tcp/64` | `2,868,938.8` | `2,891,808.0` | `-0.79%` |
| `MULTI_DEALER_DEALER/tls/64` | `3,011,233.6` | `3,001,245.2` | `+0.33%` |
| `MULTI_DEALER_DEALER/ws/64` | `2,974,515.2` | `2,975,048.8` | `-0.02%` |
| `MULTI_DEALER_DEALER/wss/64` | `3,107,328.0` | `3,105,319.4` | `+0.06%` |
| `MULTI_PUBSUB/tcp/64` | `2,415,014.6` | `2,457,731.4` | `-1.74%` |
| `MULTI_PUBSUB/tls/64` | `2,283,174.8` | `2,257,795.2` | `+1.12%` |
| `MULTI_PUBSUB/ws/64` | `2,061,065.4` | `2,025,718.0` | `+1.74%` |
| `MULTI_PUBSUB/wss/64` | `2,496,204.6` | `2,526,497.2` | `-1.20%` |
| `MULTI_SPOT/tcp/64` | `3,908,460.0` | `3,921,700.0` | `-0.34%` |
| `MULTI_SPOT/tls/64` | `5,736,198.6` | `6,812,763.0` | `-15.80%` |
| `MULTI_SPOT/ws/64` | `6,803,757.8` | `5,758,530.2` | `+18.15%` |
| `MULTI_SPOT/wss/64` | `5,615,834.6` | `6,975,371.6` | `-19.49%` |

- 판정:
  - 12개 항목 평균은 `-1.50%`로 플러스가 아니다.
  - `SPOT/ws`는 좋아졌지만 `SPOT/tls,wss`가 크게 하락했고, `PUBSUB/tcp,wss`도 원복보다 낮다.
  - "하락 항목 없이 플러스면 채택" 기준에 실패한다.
  - 테스트 전용 fault hook의 의미를 바꾸는 비용을 감수할 성능 근거가 없으므로 후보는 배제한다.
  - source 변경은 원복되어 최종 diff에 남지 않는다.
- 원복 상태 focused CTest:
  - command:
    `ctest --test-dir core/build --output-on-failure -R 'test_(helper_send_part_basic|helper_recv_part_basic|helper_ownership|helper_interleave|helper_more_bad_send|helper_request_sequence_failure|reconnect_options|spot_pubsub_scenario|spot_poller|spot_runtime_activation|spot_dispatch_event|spot_router_channel_peer|transport_matrix|multi_socket_contract_regressions|pubsub|pubsub_filter_xpub|xpub_nodrop)$|unittest_spot_data_plane_'`
  - result:
    19/19 통과.

## May 26 실행 형태 확인

- 목적:
  - corrected full baseline은 `runs=1`, `msg_sizes=64,256,1024,65536,131072,262144`,
    `connect_ready_timeout_ms=1000` 형태였다.
  - 현재 5-run 64B targeted 방식만으로 May 26과 다르게 보이는지 확인한다.
- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64,256,1024 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tls,ws,wss --duration 5 --runs 1 --connect-ready-timeout-ms 1000 --results-tag round65_spot_non_tcp_may26_shape_check`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_083800_round65_spot_non_tcp_may26_shape_check.txt`
- load_avg:
  `20.99 11.27 6.18`

| 항목 | corrected full | shape check | 차이 |
|------|----------------|-------------|------|
| `MULTI_SPOT/tls/64` | `5,939,903.4` | `3,496,286.4` | `-41.14%` |
| `MULTI_SPOT/tls/256` | `6,491,016.6` | `3,487,950.2` | `-46.26%` |
| `MULTI_SPOT/tls/1024` | `6,636,049.2` | `4,382,902.8` | `-33.95%` |
| `MULTI_SPOT/ws/64` | `5,788,890.8` | `4,293,756.2` | `-25.83%` |
| `MULTI_SPOT/ws/256` | `4,311,940.0` | `3,355,626.8` | `-22.18%` |
| `MULTI_SPOT/ws/1024` | `3,627,220.0` | `3,336,621.8` | `-8.01%` |
| `MULTI_SPOT/wss/64` | `6,776,300.6` | `3,338,143.4` | `-50.74%` |
| `MULTI_SPOT/wss/256` | `6,568,259.8` | `3,536,402.0` | `-46.16%` |
| `MULTI_SPOT/wss/1024` | `5,949,680.8` | `4,335,905.8` | `-27.12%` |

- 판정:
  - 시작 load average가 높아 절대값 판정에는 주의가 필요하다.
  - 그래도 May 26 corrected full의 6M대 SPOT non-tcp 형태는 현재 runtime에서 재현되지 않았다.
  - 실행 형태 차이만으로 gap을 설명하지 않는다.

## A/B: socket poller pre-check 제거

- 후보:
  - `a5175e0bb perf(core): reduce poller and spot hot-path overhead`에서
    `socket_poller_t::wait`가 OS poll 전에 socket events를 먼저 확인하는 경로를 추가했다.
  - SPOT data-plane event 순서에 영향을 줄 수 있어 pre-check 블록만 제거해 검증했다.
- source 상태:
  - CTest 실패 후 source 변경은 되돌렸다.
- 검증:
  - `cmake --build core/build -j$(nproc)` 통과.
  - `ctest --test-dir core/build --output-on-failure -R 'test_(reconnect_options|spot_pubsub_scenario|spot_poller|spot_runtime_activation|spot_dispatch_event|spot_router_channel_peer|transport_matrix|multi_socket_contract_regressions|pubsub|pubsub_filter_xpub|xpub_nodrop|timer_poller)$|unittest_(poller|spot_data_plane_)'`
    실패.
- 실패:
  - `test_spot_poller`
    - `test_spot_poller_wait_returns_promptly_after_reply`
    - `test_spot_poller_wait_returns_for_each_reply_in_sustained_request_loop`
  - `test_timer_poller`
    - `test_timer_poller_and_recv`
- 판정:
  - pre-check는 poller/timer/spot readiness 기능에 필요하다.
  - 성능 측정 전에 기능 회귀가 나므로 이 후보는 배제한다.

## 판정 기준 보정

- corrected baseline은 May 26 full refresh 결과를 기준으로 한다.
- 회귀 판정:
  - `-10%` 이내 하락은 측정 환경 차이로 볼 수 있어 회귀로 단정하지 않는다.
  - `-10%` 초과 `-20%` 이내 하락은 관찰/재측정 대상으로 둔다.
  - `-20%` 이상 하락은 회귀 후보로 보고 원인을 추적한다.
- 개선 채택:
  - targeted A/B에서 `+5%` 이상이면 개선 후보로 인정한다.
  - 단일 transport만 좋아지고 다른 transport가 유의미하게 나빠지면 그대로 채택하지 않는다.
  - 유지하려면 반복 측정 또는 adjacent full set에서 개선 방향이 유지되어야 한다.

## A/B: publish ingress staging 복원

- 후보:
  - `drain_publish_ingress_queue`에서 현재는 staged backlog가 없으면 ingress queue 메시지를
    바로 `forward_local_fanout`/`forward_mesh_pub`로 전달한다.
  - May 26 형태처럼 ingress queue 메시지를 먼저 `stage_message`로 staged queue에 넣고
    `flush_staged_messages`로 보내는 경로를 임시 복원했다.
- source 상태:
  - A/B 이후 source 변경은 되돌렸다.
  - 원복 후 `cmake --build core/build -j$(nproc)`를 다시 실행해 runtime도 원래 코드로 되돌렸다.
  - clock skew 경고가 한 번 있어 같은 build 명령을 재실행했고 정상 완료했다.
- 기능 검증:
  - `ctest --test-dir core/build --output-on-failure -R 'test_(spot_pubsub_scenario|spot_poller|spot_runtime_activation|spot_dispatch_event|spot_router_channel_peer|transport_matrix|multi_socket_contract_regressions|pubsub|pubsub_filter_xpub|xpub_nodrop)$|unittest_spot_data_plane_'`
  - 통과: 12/12.
- perf command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_spot_non_tcp64_staged_ingress_ab`
- report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_084902_round65_spot_non_tcp64_staged_ingress_ab.txt`
- load_avg:
  - `22.58 17.39 11.41`

| 항목 | current standalone | staged ingress A/B | 차이 |
|------|--------------------|--------------------|------|
| `MULTI_SPOT/tls/64` | `3,369,314.0` | `4,029,722.0` | `+19.60%` |
| `MULTI_SPOT/ws/64` | `4,052,109.2` | `3,447,303.4` | `-14.93%` |
| `MULTI_SPOT/wss/64` | `3,474,999.2` | `4,260,342.2` | `+22.60%` |

- corrected full baseline 대비:
  - `MULTI_SPOT/tls/64`: `-32.16%`
  - `MULTI_SPOT/ws/64`: `-40.45%`
  - `MULTI_SPOT/wss/64`: `-37.13%`
- 판정:
  - tls/wss는 `+5%` 개선 후보 기준을 넘었다.
  - ws가 `-14.93%`로 유의미하게 나빠져 전체 SPOT non-tcp 개선으로는 채택하지 않는다.
  - 이 결과는 direct forwarding과 staged flushing 사이에 transport별 tradeoff가 있음을 보여준다.
    다음 후보는 ws 손해 없이 tls/wss 이득을 가져갈 수 있는 더 작은 병목으로 좁힌다.

## A/B: direct publish drain 후 poller interest refresh

- 후보:
  - staged 경로는 `flush_staged_messages` 끝에서 `refresh_poller_interest`를 호출한다.
  - direct drain 성공 경로는 바로 `return 0`이므로 이 차이만 한 줄로 확인했다.
- source 상태:
  - A/B 이후 source 변경은 되돌렸다.
  - 원복 후 `cmake --build core/build -j$(nproc)`를 다시 실행해 runtime도 원래 코드로 되돌렸다.
- 기능 검증:
  - `cmake --build core/build -j$(nproc)` 통과.
  - `ctest --test-dir core/build --output-on-failure -R 'test_(spot_pubsub_scenario|spot_poller|spot_runtime_activation|spot_dispatch_event|spot_router_channel_peer|transport_matrix|multi_socket_contract_regressions|pubsub|pubsub_filter_xpub|xpub_nodrop)$|unittest_spot_data_plane_'`
  - 통과: 12/12.
- perf command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_spot_non_tcp64_direct_refresh_ab`
- report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_085823_round65_spot_non_tcp64_direct_refresh_ab.txt`
- load_avg:
  - `33.36 19.24 12.99`

| 항목 | current standalone | direct refresh A/B | 차이 |
|------|--------------------|--------------------|------|
| `MULTI_SPOT/tls/64` | `3,369,314.0` | `4,097,045.6` | `+21.60%` |
| `MULTI_SPOT/ws/64` | `4,052,109.2` | `3,452,747.6` | `-14.79%` |
| `MULTI_SPOT/wss/64` | `3,474,999.2` | `4,053,286.2` | `+16.64%` |

- 판정:
  - 최초 standalone 기준으로는 tls/wss가 `+5%` 개선 후보 기준을 넘고 ws가 나빠진 것처럼 보였다.
  - 그러나 바로 뒤의 원복 재측정에서 current 자체가 더 높게 나왔다.
  - 따라서 단순 poller refresh만으로는 개선이라고 볼 수 없다.

## 원복 상태 재측정

- 목적:
  - A/B 측정 중 load average가 높아져, ws 하락과 tls/wss 상승이 코드 영향인지 환경 영향인지 확인한다.
- source 상태:
  - source 변경 없음.
  - `cmake --build core/build -j$(nproc)`로 원래 코드 runtime을 다시 만들었다.
- perf command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_spot_non_tcp64_current_after_ab`
- report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_090644_round65_spot_non_tcp64_current_after_ab.txt`
- load_avg:
  - `9.86 12.66 11.57`

| 항목 | current after A/B | corrected full | 차이 |
|------|-------------------|----------------|------|
| `MULTI_SPOT/tls/64` | `4,248,429.4` | `5,939,903.4` | `-28.48%` |
| `MULTI_SPOT/ws/64` | `3,524,796.2` | `5,788,890.8` | `-39.11%` |
| `MULTI_SPOT/wss/64` | `4,263,941.0` | `6,776,300.6` | `-37.07%` |

| 항목 | current after A/B | direct refresh A/B | 차이 |
|------|-------------------|--------------------|------|
| `MULTI_SPOT/tls/64` | `4,248,429.4` | `4,097,045.6` | `-3.56%` |
| `MULTI_SPOT/ws/64` | `3,524,796.2` | `3,452,747.6` | `-2.04%` |
| `MULTI_SPOT/wss/64` | `4,263,941.0` | `4,053,286.2` | `-4.94%` |

- 판정:
  - direct refresh A/B는 인접 원복 기준으로 개선이 아니다.
  - SPOT non-tcp는 여전히 corrected full 대비 `-20%` 이상 낮아 회귀 후보로 남는다.
  - May 26 코드도 `publish_ingress_drain_batch_limit=2048`와 bytes limit `16MiB`를 이미 사용했다.
    batch limit 자체는 회귀 원인 후보에서 제외한다.

## May 26 commit 동일 환경 재현

- 목적:
  - corrected full baseline이 현재 환경에서도 재현되는지 확인해, 회귀가 코드 때문인지 측정 환경 때문인지 분리한다.
- worktree:
  - `/tmp/zlink-perf-1b60`
  - commit: `1b60c0159`
- build:
  - `cmake -S core -B core/build -DZLINK_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release`
  - `cmake --build core/build -j$(nproc)`
- perf command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --pattern SPOT --transports tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_old_1b60_spot_non_tcp64_same_env`
- report:
  - `/tmp/zlink-perf-1b60/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_091547_round65_old_1b60_spot_non_tcp64_same_env.txt`
- load_avg:
  - `27.69 12.97 10.67`

| 항목 | old commit same env | current after A/B | 차이 |
|------|---------------------|-------------------|------|
| `MULTI_SPOT/tls/64` | `5,670,895.2` | `4,248,429.4` | `-25.08%` |
| `MULTI_SPOT/ws/64` | `5,742,020.0` | `3,524,796.2` | `-38.61%` |
| `MULTI_SPOT/wss/64` | `5,639,930.8` | `4,263,941.0` | `-24.40%` |

- corrected full baseline 대비 old commit same env:
  - `MULTI_SPOT/tls/64`: `-4.53%`
  - `MULTI_SPOT/ws/64`: `-0.81%`
  - `MULTI_SPOT/wss/64`: `-16.77%`
- 판정:
  - May 26 기준은 현재 환경에서도 대체로 재현된다.
  - 현재 `main`의 SPOT non-tcp 64B 하락은 실제 core 회귀로 본다.
  - 다음 단계는 `/tmp/zlink-perf-1b60` worktree에서 commit 범위를 좁히는 것이다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거: A/B는 SPOT publish ingress forwarding 순서만 임시 변경했고,
  WS/WSS pending copy 제거, mtrie non-recursive matching, port parsing, IPC unlink order,
  decoder/message/send guards, max message size 항목을 건드리지 않았다.
- 추가로 실행한 회귀 테스트:
  - `cmake --build core/build -j$(nproc)` 통과.
  - `ctest --test-dir core/build --output-on-failure -R 'test_(spot_pubsub_scenario|spot_poller|spot_runtime_activation|spot_dispatch_event|spot_router_channel_peer|transport_matrix|multi_socket_contract_regressions|pubsub|pubsub_filter_xpub|xpub_nodrop)$|unittest_spot_data_plane_'`
    통과: 12/12.

## 판정 기준 보정

- 사용자가 지정한 corrected 기준:
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- 2026-05-13 파일은 테스트 기준이 달랐을 가능성이 있어 historical 참고로만 둔다.
- 개선 판정:
  - `+5%` 이상이면 개선 후보로 본다.
  - `-5%`에서 `+5%`는 동급 또는 노이즈로 본다.
  - `-10%` 이하는 회귀 의심으로 본다.
  - `-20%` 초과 하락은 source 원인 후보를 찾는다.
- 최종 유지 판단은 같은 조건 반복 측정과 관련 CTest 통과를 함께 본다.

## 7c168fdd4 회귀 범위 확인

- good 후보:
  - `c258bbe81`: `MULTI_SPOT/ws/64 = 5,778,385.2`
    - report:
      `/tmp/zlink-perf-1b60/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_092455_round65_bisect_c258_spot_ws64_rebuild.txt`
  - `a5175e0bb`: `MULTI_SPOT/ws/64 = 6,980,681.8`
    - report:
      `/tmp/zlink-perf-1b60/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_092702_round65_bisect_a517_spot_ws64.txt`
- bad 후보:
  - `7c168fdd4`: `MULTI_SPOT/ws/64 = 3,295,470.4`
    - report:
      `/tmp/zlink-perf-1b60/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_092914_round65_bisect_7c168_spot_ws64.txt`
- 판정:
  - `7c168fdd4 perf(core): reduce spot multipart hot-path overhead`가 SPOT/ws/64 회귀를 도입했다.

## 변경: logical pubsub queue와 part-helper storage 복원

- source:
  - `core/src/runtime/services/spot/runtime/spot_handle.hpp`
  - `core/src/runtime/services/spot/node/spot_node_pubsub_fanout.cpp`
  - `core/src/api/spot/core/service_spot_api.cpp`
  - `core/src/runtime/services/spot/pubsub/spot_subject_query.cpp`
  - `core/src/api/socket/part_helper_api.cpp`
  - `core/src/runtime/services/spot/node/spot_node.cpp`
  - `core/src/runtime/services/spot/node/spot_node.hpp`
  - `core/src/runtime/services/spot/pubsub/spot_pub.cpp`
  - `core/src/runtime/services/spot/pubsub/spot_pub.hpp`
  - `core/src/runtime/services/spot/pubsub/spot_sub.cpp`
  - `core/src/runtime/services/spot/pubsub/spot_sub.hpp`
- 내용:
  - logical local pubsub queue payload를 `zlink_msg_t` 보유 deque에서 `std::vector<std::string>`으로 되돌렸다.
  - SPOT facade/pub/sub/node 안에 part-helper state를 저장하던 경로를 제거하고, service handle은 기존 전역 map 경로를 다시 사용하게 했다.
- 근거:
  - logical queue만 복원한 probe:
    - report:
      `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_093310_round65_fix_logical_queue_strings_spot_ws64_probe.txt`
    - `MULTI_SPOT/ws/64 = 4,002,189.8`
  - 7c 임시 worktree에서 SPOT/part-helper 묶음을 부모 상태로 되돌린 A/B:
    - report:
      `/tmp/zlink-ab-7c/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_094606_round65_ab_7c_parent_spot_parthelper_group_ws64.txt`
    - `MULTI_SPOT/ws/64 = 6,869,375.6`

## 검증: SPOT non-tcp 64B

- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- CTest:
  - command:
    `ctest --test-dir core/build --output-on-failure -R 'test_(helper_send_part_basic|helper_recv_part_basic|helper_ownership|helper_interleave|helper_more_bad_send|helper_request_sequence_failure|spot_pubsub_scenario|spot_poller|spot_runtime_activation|spot_dispatch_event|spot_router_channel_peer|transport_matrix|multi_socket_contract_regressions|pubsub|pubsub_filter_xpub|xpub_nodrop)$|unittest_spot_data_plane_'`
  - result: 18/18 통과.
- ws probe:
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_095113_round65_fix_logical_queue_global_parthelper_spot_ws64_probe.txt`
  - `MULTI_SPOT/ws/64 = 6,821,720.0`
- non-tcp full:
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_095232_round65_fix_logical_queue_global_parthelper_spot_non_tcp64_full.txt`

| 항목 | corrected full | current after fix | 차이 |
|------|----------------|-------------------|------|
| `MULTI_SPOT/tls/64` | `5,939,903.4` | `5,787,464.2` | `-2.57%` |
| `MULTI_SPOT/ws/64` | `5,788,890.8` | `5,741,427.8` | `-0.82%` |
| `MULTI_SPOT/wss/64` | `6,776,300.6` | `5,729,065.6` | `-15.45%` |

- wss standalone:
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_095856_round65_fix_logical_queue_global_parthelper_spot_wss64_standalone.txt`
  - `MULTI_SPOT/wss/64 = 5,745,430.8`
- 판정:
  - tls/ws는 corrected full 대비 동급이다.
  - wss는 corrected full 단일 기준 대비 관찰 구간이지만, 같은 환경에서 재현한 old commit `5,639,930.8`보다 높다.
  - SPOT non-tcp 회귀는 이번 변경으로 큰 폭 회복했다.

## 확인: STREAM tcp 64B

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round65_fix_logical_queue_global_parthelper_stream_tcp64_check`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_100123_round65_fix_logical_queue_global_parthelper_stream_tcp64_check.txt`
- result:
  - `MULTI_STREAM/tcp/64 = 327,775.2`
- 기준 비교:
  - smoke baseline `325,470.0` 대비 `+0.71%`
  - full baseline `305,177.4` 대비 `+7.40%`
- 판정:
  - `+5%` 기준으로는 full baseline 대비 개선이다.
  - 사용자가 말한 400kops 목표에는 아직 부족하다.
  - 다음 단계는 STREAM tcp/64 hot path를 별도 후보로 본다.

## 축소 full 64B 확인

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round65_fix_spot_all64_reduced_full`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_100434_round65_fix_spot_all64_reduced_full.txt`
- completion:
  `success=32`, `fail=0`, `status=complete`

### 문제 report 대비

- 공통 64B 항목: 26개.
- 전체 평균: `+1.29%`
- 전체 중앙값: `+2.67%`
- one-way 평균: `+0.93%`
- one-way 중앙값: `-4.65%`
- echo 평균: `+1.52%`
- echo 중앙값: `+3.78%`

| 항목 | problem | current | 차이 |
|------|---------|---------|------|
| `MULTI_SPOT/tls/64` | `3,739,003.6` | `5,615,676.4` | `+50.19%` |
| `MULTI_STREAM/tcp/64` | `299,395.0` | `322,828.2` | `+7.83%` |
| `MULTI_DEALER_ROUTER/tls/64` | `371,483.6` | `398,474.2` | `+7.27%` |
| `MULTI_ROUTER_ROUTER/wss/64` | `337,912.6` | `361,466.4` | `+6.97%` |
| `MULTI_SPOT_SENDSEND/tcp/64` | `247,978.4` | `191,079.0` | `-22.95%` |
| `MULTI_SPOT_SENDSEND/tls/64` | `236,013.6` | `195,541.8` | `-17.15%` |
| `MULTI_PUBSUB/tcp/64` | `2,628,104.8` | `2,365,890.2` | `-9.98%` |
| `MULTI_PUBSUB/tls/64` | `2,446,707.8` | `2,282,125.0` | `-6.73%` |

### May26 full 대비

- 공통 64B 항목: 32개.
- 전체 평균: `-2.29%`
- 전체 중앙값: `-0.05%`
- worst:
  - `MULTI_SPOT_SENDSEND/tcp/64`: `-29.54%`
  - `MULTI_SPOT_SENDSEND/tls/64`: `-23.02%`
  - `MULTI_PUBSUB/tls/64`: `-13.00%`
  - `MULTI_SPOT_SENDSEND/ws/64`: `-11.85%`
  - `MULTI_PUBSUB/tcp/64`: `-11.11%`

- 판정:
  - SPOT non-tcp 회귀는 회복했지만 round 목표인 전체 64B 평균/중앙값 개선은 아직 미달이다.
  - 다음 후보는 one-way 평균에 직접 영향을 주는 `MULTI_PUBSUB/tcp,tls`다.
  - `MULTI_SPOT_SENDSEND/tcp,tls`도 큰 미달이지만 echo 계열이라 PUBSUB 다음 후보로 둔다.
