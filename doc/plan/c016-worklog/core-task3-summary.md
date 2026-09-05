# Core task 3 — reciprocal ROUTER HANDOVER 계약 조사

## 판정

Core는 현재 spec대로 동작한다. Reciprocal RID 충돌 시 `A < Z` 비교 결과로 양쪽이 동일한
`A -> Z` 방향을 active route로 선택한다. 패배한 `Z -> A`의 Application/Completion 두 lane은
handover 시 종료되지 않고 active duplicate standby로 유지된다. 이는 spec이 명시적으로 허용한
동작이므로 Core 소스는 수정하지 않았다.

## 시나리오별 결과

REQUEST timeout은 각 cell에서 600 ms로 설정했다. `standby 관찰`은 handover 시작부터 명시적으로
패배 connector를 해제하기 직전까지이며, 이 구간에서 양쪽 monitor의 arbitration
`DISCONNECTED`와 `CLOSED`는 모두 0회였다.

| transport | RECONNECT_IVL | standby 관찰 | loser timeout | arbitration A (`DISCONNECTED/CLOSED`) | arbitration Z (`DISCONNECTED/CLOSED`) | winner-first | loser 해제 뒤 retry |
|---|---:|---:|---:|---:|---:|---|---|
| tcp | 10 ms | 649 ms | 600 ms | 0 / 0 | 0 / 0 | OK | OK |
| tcp | 100 ms | 650 ms | 601 ms | 0 / 0 | 0 / 0 | OK | OK |
| tcp | 1000 ms | 2000 ms | 600 ms | 0 / 0 | 0 / 0 | OK | OK |
| inproc | 10 ms | 651 ms | 600 ms | 0 / 0 | 0 / 0 | OK | OK |
| inproc | 100 ms | 651 ms | 600 ms | 0 / 0 | 0 / 0 | OK | OK |
| inproc | 1000 ms | 2000 ms | 601 ms | 0 / 0 | 0 / 0 | OK | OK |

- (a) 패배 lane은 즉시, linger, reconnect timer 어느 시점에도 자동 종료되지 않았다. 관찰 상한은
  interval 10/100 ms cell에서 약 650 ms, interval 1000 ms cell에서 2000 ms였다. TCP에서는 이후
  패배 connector를 명시 해제했을 때 양쪽 monitor가 Application/Completion `DISCONNECTED`를 각
  2회 관찰했다. inproc은 physical `DISCONNECTED/CLOSED` event를 내지 않지만 공개 context budget
  snapshot에서 Application directional queue 2개와 Completion directional queue 2개가 함께
  감소해 두 lane 종료를 확인했다.
- (b) `A -> Z`를 active로 선택한 뒤 패배 connector `Z -> A`만 명시 해제해도 Z의 REQUEST와 A의
  Completion reply가 성공했다. 양쪽이 동일한 남은 physical pair를 사용함을 확인했다.
- (c) 패배 `Z -> A`가 sole route일 때 admit되어 A에서 실제 수신된 REQUEST는 handover 뒤 600~601
  ms에 `ZLINK_REQUEST_TIMED_OUT` completion 정확히 1회로 끝났다. 패배 pair 해제 뒤 새 REQUEST
  재전송은 승자 `A -> Z` pair에서 reply까지 성공했다.
- (d) 승자 pair attach 뒤 패배 pair가 여전히 standby로 존재하고 monitor 종료 event가 0인 상태에서
  제출한 첫 REQUEST가 정상 reply completion으로 끝났다.

## Spec 대조

- `core/doc/spec/core/socket/README.ko.md:145-165` §4: HANDOVER의 reciprocal 충돌은 두 RID를
  비교해 양쪽이 같은 한 방향을 고르고, 물러나는 방향에 admit된 request는 선택 방향으로 이어지지
  않으며 자기 timeout으로 정확히 한 번 끝난다.
- `core/doc/spec/core/socket/07-router.ko.md:153-155` §5: active duplicate는 standby 동안 상태를
  보관하고 나중에 같은 pipe가 선택되면 사용한다. 따라서 loser lane 자동 종료는 계약 요구가 아니다.
- `core/doc/spec/core/protocol/01-zmp.ko.md:176-205` §4.1: ROUTER-ROUTER는 Application lane 0과
  Completion lane 1의 두 physical connection을 사용한다. Application은 DATA/REQUEST를,
  Completion은 REPLY/FLOWSTATE를 운반한다.
- `core/doc/spec/core/06-monitoring.ko.md:67-77,281-290` §3.1/§7.3: `transport_lane`은 physical
  connection을 분류하고 ROUTER-ROUTER의 두 lane이 모두 준비되어야 logical ready가 된다. 공개 enum은
  Application=0, Completion=1이다.

## 원인과 구현 위치

- 방향 결정: `core/src/runtime/sockets/router/router_admission.cpp:302-335`가 local/peer RID를 byte
  비교하고 작은 RID 쪽이 locally initiated한 direction을 승자로 만든다.
- standby 유지: 같은 파일 `:364-379`는 새 loser를, `:398-416`은 교체된 reciprocal loser를 synthetic
  RID로 route map에 남기고 `_standby_pipes`에 원래 RID를 기록한다. terminate action을 만들지 않는다.
- REQUEST pair fence: `core/src/api/socket/socket_request_reply_submit_api.cpp:121-180`이 admit 시점의
  pair ID/generation과 correlation pipe를 pending에 고정한다. `socket_request_reply_pending_api.cpp:89-158`
  은 다른 pair/generation의 reply가 pending을 소비하지 못하게 한다.
- 단일 timeout: `core/src/api/socket/socket_request_reply_internal.cpp:532-547`가 pending 제거에 성공한
  timeout owner만 `ZLINK_REQUEST_TIMED_OUT` completion을 발행한다.

## 수정

- `core/tests/integration/test_router_reciprocal_handover_lanes.cpp`: 공개 C API만 사용한 tcp/inproc ×
  RECONNECT_IVL 10/100/1000 ms 계약 테스트 추가. 두 lane attach, monitor 종료 부재, 동일 승자 방향,
  loser 단일 timeout, winner-first, loser 해제 뒤 retry를 검증하고 cell별 측정값을 출력한다.
- `core/tests/CMakeLists.txt:117,361-365`: `test_router_reciprocal_handover_lanes` 등록, TIMEOUT 60.
  공통 등록 경로 `:943-945`를 통해 labels는 `integration;serial`이다.
- Core runtime 소스 수정 없음. 금지된 spec/framework/doc/plan 경로 수정 없음.

## 회귀 테스트와 gate

- 회귀 테스트: `test_router_reciprocal_handover_lanes`
  - `test_reciprocal_handover_tcp_{10,100,1000}ms`
  - `test_reciprocal_handover_inproc_{10,100,1000}ms`
- configure: `core/build-task3`, `RelWithDebInfo`, `ENABLE_LTO=OFF`, `ZLINK_BUILD_TESTS=ON`,
  `BUILD_TESTS=ON`.
- build: `cmake --build core/build-task3 -j2` — 100% 성공.
- 새 테스트 반복: `--repeat until-fail:5` — 5/5 green, 각 실행 6/6 scenario, 약 7.02 s/run.
- 간헐 경로 분리 확인: TCP/100 ms 단독 20/20 green.
- integration gate: `ctest --test-dir core/build-task3 -L integration -j2 --output-on-failure` —
  92/92 green, 최종 코드 기준 159.81 s.
- `git diff --check` — green.
- 전체 ctest는 Core runtime 소스를 수정하지 않아 요청 조건상 생략했다. `hotpath_gate`는 감독자 별도.

## BLOCKERS

없음.

spec대로 동작 확인 (test_router_reciprocal_handover_lanes)
