# ToActorMessaging feature-map

이 스위트는 공통 config-9 `to-actor messaging`의 C++ 구현 위치다.

- public 표면: `zlink::framework::actor_client_t`
- send 터미널: `send_to_actor(...).async()`
- request 터미널: `request_to_actor(...).async<TReply>()`
- 실패 분류: `actor_route_not_found`, `actor_location_stale`, `route_not_connected`

이전에 전체 runner가 통과한 proof는 `logs/20260716-091927-3274099`에 남아 있다. 그러나 현재
placement와 actor lifecycle 변경 이후에는 각 scenario를 다시 검증해야 하며, 이전 proof만으로 현재
구현 완료를 판정하지 않는다.

| 공통 항목 | 상태 | C++ 구현 |
|-----------|------|----------|
| TA-A1 bind된 actor send/request | implemented | `session-a` connector가 bind 전후 push를 받고, caller의 send/request 뒤에도 bind evidence가 한 건으로 유지되는지 확인한다. |
| TA-A2 bind 안 된 actor send/request | implemented | 두 session gateway에 bind evidence가 없는 상태에서 actor mailbox send와 request reply를 확인한다. |
| TA-A3 no-bind 전달 뒤 이후 bind | implemented | bind 전 send/request 뒤 `session-b`에 bind하고, bind 후 send/request와 `LateBindNotify` push를 확인한다. |
| TA-A4 unbind/disconnect 후 | implemented | connector disconnect evidence 뒤 같은 actor 호출이 성공하고, 명시적 destroy 뒤 같은 id request가 `actor_route_not_found`인지 확인한다. |
| TA-B1 row 없음 | implemented | `TA-B1-missing*`가 send/request 모두 `actor_route_not_found`를 반환하고 역할 서버 evidence를 만들지 않는지 검증한다. |
| TA-B2 stale location | implementation gap | Actor process의 ensure fixture를 `actor_manager_t` public creation 경로로 이관하고 destroy callback의 stateful reservation 정리를 보강했다. 이전 재검증은 `Actor placement candidates were exhausted`로 종료됐으며(`logs/20260806-150955-2682657`), 수정 후 재검증은 Redis readiness timeout으로 종료되어 placement capacity 회수와 이전 `ActorRef` stale 검증을 아직 완료하지 못했다. |
| TA-B3 route not connected | 전환 필요 | 현재 source는 정식 C++ interface에 없는 `router_connections()`를 사용한다. `mesh.peer_connections()`의 public `disconnect(endpoint)`와 `connect(endpoint)`로 두 actor endpoint를 제거·복구하고, 저장한 ref의 `route_not_connected`, 복구 뒤 같은 ref와 actor-a evidence를 다시 검증해야 한다. |

`run_e2e.sh`는 Redis를 준비한 뒤 actor owner 두 개, caller, session gateway 두 개를 시작하고 모든 health를
기다린 다음 client runner를 실행한다. `E2E_START_ORDER=reverse`에서도 같은 순서 독립성을 검증한다.
현재 C++ runner는 전용 Docker Redis를 직접 시작하며, 사용자 환경에서 전달한 외부 Redis endpoint를
공유 Redis로 재사용하지 않는다.
