# Core disconnect-progress 간헐 실패 진단

## 결과

두 실패는 Core의 별도 기존 결함(B)이다. inproc은 앞서 queued된 bind를 public disconnect가
지나쳐 종료할 pipe를 놓쳤다. STREAM은 실제 연결 전에 만든 pipe에서 connection ID 0인
READY를 발행했다. Monitor queue의 병합이나 `wait_monitor_event`의 과거 event 누락이 원인은 아니다.

수정한 공개 C API 테스트에서 요청한 두 사례가 **각각 200/200 PASS**했다.
`hotpath_gate` 4개 cell도 PASS했다. Integration은 **127/128 PASS**이며,
기존 `test_flow_state_paired` 실패 때문에 **전체 lane 0 failures 조건은 미충족**이다.
해당 실패는 수정 전 library에서도 재현했고, 아래에 독립 증거를 남겼다.

- 기준: detached `origin/main` `0c28961cebcc12ef146f34862d924e310d3ff989`.
- 구현·빌드·실행 위치: `/home/hep7/project/zlink-core-gate`.
- [적용할 패치](/tmp/zlink-core-gate/disconnect-progress-fix.patch): 3개 파일, 41행 추가·21행 삭제.
- 패치 SHA-256: `63ee16c75376cd1f8157dac853c930e74ad534b2936a4fb61fb00c9c8fbd03d7`.
- Commit·push·spec 수정 없음. Main에는 요청한 이 결과 파일만 생성했다.

## inproc: queued bind를 지나친 endpoint 제거

기준 revision의 원인 위치는 다음과 같다.

1. `core/src/runtime/sockets/common/socket_base_endpoint.cpp:436`은 peer socket mailbox에
   bind를 보내고, `:468–470`은 connector의 pipe와 READY를 공개한다. Peer의 bind 실행은
   아직 끝나지 않을 수 있다.
2. `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:376–384`의
   `process_commands()`는 async owner가 있으면 public 호출에서 command를 처리하지 않고
   성공을 반환한다. `:422`의 두 번째 검사도 같다.
3. `core/src/runtime/sockets/common/socket_base_endpoint.cpp:1125–1132`의
   `term_endpoint()`는 이미 API 직렬화 잠금을 가지고 있는데도 이 생략 경로를 탄다.
   `:1034–1048`은 endpoint 등록을 제거한 뒤 **이미 attached된 pipe만** 종료한다.
   아직 mailbox에 있는 bind는 이 목록에 없다.
4. API 잠금이 풀린 뒤 `socket_base_lifecycle.cpp:1358–1365`가 늦은 bind를 처리한다.
   Socket 자체는 close되지 않았으므로 pipe가 활성 상태로 등록되고 DISCONNECTED는 나오지 않는다.

첫 독립 재현은 기존 `ZLINK_DEBUG_PIPE_TERM=1`과 event 수신 로그를 켠 36번째 실행이었다.
[실패 로그](/tmp/zlink-core-gate/baseline/test_inproc_unregistered_server_disconnect_progresses-36.log)에는
READY(connection 5) 뒤 3초 동안 종료 시작 로그가 없고, assertion 실패 후 정리에서야 pipe 종료가 나타난다.

추가로 hot path에 파일 출력을 넣지 않고 메모리에 등록·제거·event 이력을 기록했다.
첫 연결만 분리한 진단 executable의 1,935번째 실행에서 같은 실패를 자연 재현했다.
[원시 이력](/tmp/zlink-core-gate/ring-first/inproc-1935.log)의 관련 순서는 다음과 같다.

| 순번 | 관찰 |
|---|---|
| 0 | Connector가 READY(connection 5)를 enqueue |
| 3 | Public monitor consumer가 READY(connection 5)를 수신 |
| 4 | Server `0x561d2b558780`의 disconnect 진입: attached pipe 0, async owner 활성 |
| 5 | Server endpoint 등록 제거: attached pipe 0 |
| 6 | Public disconnect 반환 |
| 7 | 같은 connection 5의 DISCONNECTED 대기 시작 |
| 8 | 같은 server가 뒤늦게 bind(connection 5)를 처리 |
| 이후 | 3초 만료. DISCONNECTED enqueue·수신 기록 없음 |

진단용 단일-cycle 실행과 최종 회귀를 구분한다. 최종 회귀는 원래 20-cycle 테스트를
200회 실행했으며, 진단용 계측과 내부 symbol은 최종 패치에 없다.

**수정:** `process_commands()`가 기존 public API 잠금을 가진 control 호출도 command 처리
소유자로 인정한다. 기존 API → command-owner → receive 잠금 순서로 앞선 command를 처리한
뒤 endpoint를 제거한다. 잠금 없는 progress 조회는 계속 async executor에 위임한다.
별도 pending-bind 목록, endpoint generation, 취소 상태 또는 재시도 경로를 만들지 않았다.

## STREAM: 물리 연결 전 READY 발행

기준 revision의 `socket_base_endpoint.cpp:645–706`은 `IMMEDIATE=0`인 첫 connect에서
session이 연결되기 전에 pipe를 만들어 socket에 attach한다. `:684–688`의 endpoint에는
connection ID 0이 들어 있다. `core/src/runtime/sockets/stream/stream.cpp:223–231`은 attach 시
RID를 만들고 READY를 발행한다. 따라서 이 READY를 받은 application은 아직 TCP accept가
완료되지 않았는데도 server endpoint를 해제할 수 있다.

[단일 I/O thread 실패 이력](/tmp/zlink-core-gate/stream-baseline/stream-286.log):
READY(event 4096, connection 0, edge flag 1) → server disconnect → DISCONNECTED 3초 만료.
이 경우 monitor에서 보인 READY가 성립한 물리 연결을 가리키지 않았다.

**수정:** STREAM의 첫 pipe도 기존 `session_base_t::engine_ready()`의 연결 후 생성·bind 경로로
보낸다(`core/src/runtime/core/session_base.cpp:480–608`).
`session_base.cpp:753–765`의 재연결 때와 같은 물리 연결 단위의 pipe 생성 규칙을 사용한다.
READY event에 0을 숨기는 조건을 추가하거나 별도 event를 합성하지 않았다.

**공개 C API 회귀 강화:** 첫 READY의 connection ID가 0이 아님을 확인하고,
그 **같은 ID**의 DISCONNECTED를 요구한다. 다음 READY는 0이 아니며 이전과 다른 ID여야 한다.
기존 fresh RID 검사도 유지한다. 강화한 검사는
[수정 전 library에서 실패](/tmp/zlink-core-gate/stream-strengthened-baseline.log)하고 수정 후 통과한다.
`ZLINK_TEST_CASE` 선택은 저장소의 기존 선택 방식과 같으며 200회 검증에서도 실행 사례 수가
정확히 1인지 확인했다.

## Monitor·helper·연관 변경 판정

- **(a) Core:** inproc 종료 대상 누락과 STREAM의 조기 READY가 확인됐다.
- **(b) Monitor 병합:** 현재 `core/doc/spec/core/06-monitoring.ko.md:100–114` §4는 bounded queue가
  가득 차면 새 record를 버리고 기존 record를 유지하며 aggregate하지 않는다고 규정한다.
  `socket_monitor_runtime.cpp:242–266`도 이 규칙이다. 이 revision의 §6.3은 byte 진단 field이며,
  D-B119(`doc/plan/c016-worklog/decisions.ko.md:1277`)는 REQUEST correlation credit의 WRITABLE
  조건에 관한 결정이다. 요청에 언급된 한 슬롯 monitor 병합 규칙과 다르다.
- **(c) Helper:** `wait_monitor_event`는 level-triggered POLLIN 뒤 DONTWAIT로 queue를 읽는다.
  대기 시작 전에 도착한 record도 읽을 수 있다. inproc 실패는 DISCONNECTED 자체가 enqueue되지
  않았고, STREAM은 잘못된 READY에서 시작했다. Helper의 소비·timeout 알고리즘은 변경하지 않았다.
- **D-118:** hold release의 byte-credit waiter는 DATA admission 진행을 소유한다. 확인한 inproc
  실패에서는 해당 server pipe가 attach조차 되지 않았고, STREAM에서는 물리 연결 전 READY가
  원인이다. Credit 재시도·HWM 수정은 하지 않았다.
- **D-098·973ebe30d5:** listener endpoint의 동기 해제, `own_t::term_child()`의 mailbox 순서,
  sequence accounting과 child/pipe ack 동안 executor 유지 규칙을 보존했다. 이번 누락은 이 규칙들
  앞의 public command drain과 첫 STREAM pipe 공개 경계에서 발생했다.

검토한 대안은 늦은 bind마다 별도 취소 상태를 유지하거나 기존 직렬화 소유자를 재사용하는
방법이었다. 후자를 선택했다. STREAM도 READY만 필터링하는 방법과 pipe 생성 시점을 소유
session으로 통일하는 방법 중 후자를 선택했다.

수정 전/후 규칙 수: control 호출의 선행 command 적용 2개(async owner 유무에 따라 적용/생략) →
1개(직렬화된 control scope에서 적용), STREAM pipe 공개 2개(첫 연결 전/재연결 후) →
1개(물리 연결 후). 합계 **4 → 2**, 새 지속 상태 없음.

## 검증

Release+LTO 설정을 유지했고 build 전마다 `pgrep -c lto1 == 0`을 확인했다.
Integration executable은 shared Core library에 연결되며 test executable 자체는 non-LTO다.

| 검증 | 결과 | 증거 |
|---|---|---|
| 관련 테스트 파일 전체 | 14개 사례 PASS | [focused log](/tmp/zlink-core-gate/fix-focused.log) |
| `test_inproc_unregistered_server_disconnect_progresses` | 200/200, 총 4,000 disconnect/rebind cycle | [집계](/tmp/zlink-core-gate/regression-200/summary.json) |
| `test_stream_disconnect_reconnect_on_single_io_thread` | 200/200, ID·RID 검사 포함 | 같은 집계 |
| `unittest_receive_transaction`, `unittest_ctx_lifecycle`, `unittest_zmp_pair_lifecycle`, `unittest_monitor_ready_drain` | 4/4 executable PASS | [unit log](/tmp/zlink-core-gate/fix-unit.log) |
| Integration lane | 127/128 PASS, 기존 peer weight 실패 1개 | [첫 구간](/tmp/zlink-core-gate/integration.log), [남은 72개 PASS](/tmp/zlink-core-gate/integration-remaining.log), [전체 집계](/tmp/zlink-core-gate/integration-results.json) |
| `hotpath_gate` | 4/4 cell PASS, reference 변경 없음 | [gate log](/tmp/zlink-core-gate/hotpath.log) |
| `git diff --check` | PASS | 최종 patch 생성 전 확인 |

원래 `event_timeout_ms=3000`, sample 수 20, disconnect p95 제한 200 ms와 reconnect p95 제한
250 ms를 유지했다. 200회 inproc 회귀의 disconnect와 reconnect는 각 실행의 p95·maximum을
통틀어 최대 10 ms였다. Timeout·retry·sleep 증가와 assertion 완화는 없다.

| Hotpath cell | Reference instructions/message | Measured | Ratio |
|---|---:|---:|---:|
| dealer_dealer_inproc | 3455.381 | 3418.898100 | 0.9894 |
| dealer_router_reqrep_inproc | 12054.8948 | 12203.958000 | 1.0124 |
| pair_inproc | 2505.36 | 2518.346700 | 1.0052 |
| router_router_tcp | 2972.8817 | 2974.028200 | 1.0004 |

선택 사례를 다시 실행하는 명령은 다음과 같다. 200회 검증은 두 명령을 각 200회 실행했다.

```bash
ZLINK_TEST_CASE=test_inproc_unregistered_server_disconnect_progresses \
  core/build-gate/bin/test_socket_disconnect_progress_without_app_poll
ZLINK_TEST_CASE=test_stream_disconnect_reconnect_on_single_io_thread \
  core/build-gate/bin/test_socket_disconnect_progress_without_app_poll
```

## 남은 실패와 소유 경계

`test_flow_state_paired`의 `test_network_peer_weight_replays_after_reconnection`은
`core/tests/integration/test_flow_state_paired.cpp:322`에서 peer weight를 3초 안에 관찰하지 못했다.
Assertion 후 정리가 끝나지 않아 lane에서는 CTest 10초 timeout으로 기록됐다.
별도 A/B 실행에서도 수정 전 library는 5회 통과 후 6번째 실패, 수정 후 library는 3회 통과 후
4번째 실패했다. [Baseline 실패](/tmp/zlink-core-gate/flow-ab/baseline-6.log),
[수정 후 실패](/tmp/zlink-core-gate/flow-ab/fixed-4.log), [A/B 집계](/tmp/zlink-core-gate/flow-ab/summary.json).
이 적은 표본으로 실패율의 증감을 판정하지 않는다.

같은 기존 실패는 `doc/plan/c016-worklog/core-tests-rebase-summary.md:113–120`에 기록돼 있고,
`briefs/diag-core-flow-state-paired-intermittent.prompt`의 별도 Core 작업 범위다.
해당 assertion·fixture·Core weight 경로는 변경하지 않았다. 이 실패가 해결되기 전에는
이번 작업으로 integration lane 전체가 green이라고 판정할 수 없다.

- **소유 계층:** Core socket의 command/lifecycle 직렬화와 session의 물리 연결 pipe 생성.
- **Spec 조항:** socket README `zlink_disconnect`(:879–884), `05-polling` §3,
  `06-monitoring` §3.1·§4·§9(:539–544), STREAM §4·§9.
- **교차언어 대조:** 공통 C API에서 검증했다. Binding·Framework runtime 변경은 없고,
  언어별 suite는 이번 Core 작업 범위에서 실행하지 않았다.
- **변경 분류:** B — 기존 Core 결함 2개 수정과 공개 C API 회귀 강화. A/C/D 변경 없음.
