# Round 63: one-way 64B pipe/fanout hot path

- goal: `MULTI_SPOT`, `MULTI_PUBSUB`, `MULTI_DEALER_DEALER` 64B one-way 회귀를 줄인다.
- 완료 기준:
  - targeted one-way 64B set에서 현재 문제 report 대비 중앙값 `+10%` 이상 또는 반복되는 큰 회귀 항목
    하나 이상이 현재 기준 대비 `+10%` 이상 개선된다.
  - `cmake --build core/build -j$(nproc)` 통과.
  - 관련 core test 통과.
  - perf runner가 `core/build` 아래 runtime을 사용했음을 로그에 남긴다.
- 시작 시각: 2026-06-15 KST
- 기준 commit: `72d893595`
- 시작 git status:
  - core/perf source diff 없음.
  - dotnet 문서 변경은 이 작업과 무관하므로 건드리지 않는다.
- 기준 report:
  - baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - problem: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 참고 current report:
  - round43 clean 64B sweep:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_025722_round43_current_64b_lowload_sweep.txt`
  - round51 one-way repeat:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_041202_round51_oneway_64b_repeat.txt`
- 대상 pattern/transport/size:
  - `MULTI_SPOT`, `MULTI_PUBSUB`, `MULTI_DEALER_DEALER`
  - `tcp,tls,ws,wss`
  - `64B`

## 기준 숫자

- problem vs baseline 64B 공통 항목:
  - 평균: `-15.6%`
  - 중앙값: `-14.9%`
- 반복되는 one-way 회귀:
  - `MULTI_SPOT`: baseline 대비 약 `-39%`에서 `-53%` 대역.
  - `MULTI_PUBSUB`: baseline 대비 약 `-22%`에서 `-32%` 대역.
  - `MULTI_DEALER_DEALER`: baseline 대비 약 `-26%` 대역.
- round43/round51에서도 위 one-way 회귀는 반복된다.

## 가설

- 가설 1: 64B one-way 경로에서 pipe enqueue/flush의 lock/HWM/activation 비용이 증가했고,
  SPOT/PUBSUB/DEALER_DEALER 모두 같은 pipe hot path를 반복해서 밟는다.
- 가설 2: SPOT/PUBSUB fanout에서 subscription 매칭 이후 같은 메시지를 여러 pipe로 보내는 과정의
  refcount 또는 copy 비용이 늘어, fanout 계열 회귀가 DEALER_DEALER보다 크게 나타난다.
- 가설 3: mailbox/wakeup 또는 poller batch 정책이 작은 메시지에서 더 자주 깨워, one-way 전체의
  context-switch 비용이 늘었다.
- 선택한 가설: 먼저 가설 1을 검증한다. DEALER_DEALER와 PUBSUB/SPOT이 공유하는 pipe send path에서
  64B 단일-part final send 비용을 줄일 수 있는지 코드 기준으로 확인한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 아직 없음.
- 보안 의미를 유지한 근거: 현재는 분석 단계이며 source 변경 없음.
- 추가로 실행한 회귀 테스트: source 후보가 생기면 기록한다.

## 읽은 코드

- `core/src/runtime/sockets/dealer/dealer.cpp`
  - `DEALER` one-way send는 `lb_t::sendpipe()`를 통해 pipe에 쓰고 flush한다.
- `core/src/runtime/sockets/internal/lb.cpp`
  - 단일 active pipe fast path가 있지만 final single-part send는 일반 `write_and_flush()`를 사용한다.
  - 일반 데이터 frame에서는 routing-id가 아니므로 더 좁은 pipe single-message fast path를 쓸 수 있다.
- `core/src/runtime/sockets/internal/dist.cpp`
  - `PUB/SUB` fanout은 `dist_t::write_at()`에서 matching pipe마다 `write_and_flush_no_recursive_hwm_check()`를 호출한다.
  - final single-part 일반 데이터 frame은 STREAM에서 이미 사용하는
    `write_single_message_and_flush_no_recursive_hwm_check()`와 같은 의미를 갖는다.
- `core/src/runtime/sockets/pubsub/xpub.cpp`
  - `XPUB` data send는 `dist_t::send_to_all()` 또는 `dist_t::send_to_matching()`으로 모인다.
- `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`
  - local fanout은 `spot_publish_msg_parts()`를 통해 pub/sub fanout path를 밟는다.

## 변경

- 변경 파일:
  - `core/src/runtime/sockets/internal/dist.cpp`
  - `core/src/runtime/sockets/internal/lb.cpp`
- 변경 이유:
  - 64B one-way 일반 데이터 frame의 final single-part send에서 `more`와 routing-id 처리를 포함한 일반
    pipe write/flush 경로 대신, 이미 STREAM hot path에서 쓰는 단일 메시지 write/flush helper를 재사용한다.
- perf 전용 변경이 아닌 이유:
  - public socket send 경로의 일반 final single-part data frame에 적용되는 core pipe enqueue hot path
    개선이다.
  - benchmark 조건이나 perf client/server 동작은 바꾸지 않는다.
- 계약 보존:
  - routing-id frame은 기존 `write_and_flush*()` 경로를 유지해 `_msgs_written` 카운팅 의미를 바꾸지 않는다.
  - multipart frame은 기존 `write()` 또는 `write_no_recursive_hwm_check()` 경로를 유지한다.

## Build와 관련 테스트

- build command:
  `cmake --build core/build -j$(nproc)`
- result:
  - 통과.
- ctest command:
  `ctest --test-dir core/build --output-on-failure -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|spot_pubsub_scenario|spot_router_channel_peer|backpressure_oneway_matrix|backpressure_matrix|multi_socket_contract_regressions|router_multiple_dealers)|unittest_spot_'`
- result:
  - 28개 중 27개 통과.
  - `test_spot_pubsub_scenario_node_child_interop` 1회 실패.
- focused rerun:
  `ctest --test-dir core/build --output-on-failure -R '^test_spot_pubsub_scenario_node_child_interop$' --repeat until-pass:3`
- focused rerun result:
  - 통과.
- 판정:
  - SPOT 인접 경로이므로 실패는 기록한다.
  - 단독 재실행 통과로 비결정 가능성은 있지만, 성능 후보가 실패하면 source 변경은 유지하지 않는다.

## Targeted perf

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,PUBSUB,SPOT --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round63_oneway64_single_msg_pipe_fastpath`
- runner runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_070628_round63_oneway64_single_msg_pipe_fastpath.txt`
- completion:
  - success: `12`
  - fail: `0`
  - status: `complete`
- 시작 load_avg:
  - `16.52 9.37 5.23`
- result summary:
  - problem 대비 평균: `-3.5%`
  - problem 대비 중앙값: `-5.3%`
  - round51 대비 평균: `+0.9%`
  - round51 대비 중앙값: `+0.4%`
- 판정:
  - 5% 미만은 오차로 보는 계획 기준상 개선 신호가 없다.
  - problem 기준 완료 조건인 중앙값 `+10%`에도 전혀 미달한다.
  - 이 변경은 유지하지 않는다.

## 원복

- 원복 파일:
  - `core/src/runtime/sockets/internal/dist.cpp`
  - `core/src/runtime/sockets/internal/lb.cpp`
- 원복 이유:
  - targeted one-way 64B set에서 개선 효과가 없고 일부 항목은 problem 대비 낮았다.
  - 효과 없는 성능 변경은 남기지 않는다는 계획 원칙을 따른다.
- 원복 후 조치:
  - 원복 뒤 `core/build` runtime이 실패 후보 빌드로 남지 않도록 다시 build한다.

## 원복 후 확인

- command:
  `cmake --build core/build -j$(nproc)`
- result:
  - 통과.
- diff 확인:
  - `git diff -- core/src core/include core/tests bindings/c/perf --stat`: 출력 없음.

## Round 63 판정

- 단일 메시지 pipe helper 재사용은 실제 64B one-way 성능 개선으로 이어지지 않았다.
- source 변경은 모두 원복했다.
- perf 전용 변경은 하지 않았다.
- 보안 하드닝 의미 변경 없음.
- 다음 후보:
  - 64B message storage/refcount 경로 확인.
  - PUBSUB/SPOT fanout에서 vsm/heap message 처리 비용이 반복 회귀를 설명하는지 본다.
