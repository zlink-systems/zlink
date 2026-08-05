# ToActorMessaging feature-map

이 스위트는 공통 config-9 `to-actor messaging`의 C++ 구현 위치다.

- public 표면: `zlink::framework::actor_client_t`
- send 터미널: `send_to_actor(...).async()`
- request 터미널: `request_to_actor(...).async<TReply>()`
- 실패 분류: `actor_route_not_found`, `actor_location_stale`, `route_not_connected`

최신 proof는 `timeout 480s framework/languages/cpp/e2e/ToActorMessaging/run_e2e.sh all`이며,
로그는 `logs/20260716-091927-3274099`이다. 이 실행은 actor owner 두 개, caller, `session-a`, `session-b`,
Redis location store와 connector client를 실행하고 `to-actor-messaging e2e result=passed`를 출력했다.

| 공통 항목 | 상태 | C++ 구현 |
|-----------|------|----------|
| TA-A1 bind된 actor send/request | implemented | `session-a` connector가 bind 전후 push를 받고, caller의 send/request 뒤에도 bind evidence가 한 건으로 유지되는지 확인한다. |
| TA-A2 bind 안 된 actor send/request | implemented | 두 session gateway에 bind evidence가 없는 상태에서 actor mailbox send와 request reply를 확인한다. |
| TA-A3 no-bind 전달 뒤 이후 bind | implemented | bind 전 send/request 뒤 `session-b`에 bind하고, bind 후 send/request와 `LateBindNotify` push를 확인한다. |
| TA-A4 unbind/disconnect 후 | implemented | connector disconnect evidence 뒤 같은 actor 호출이 성공하고, 명시적 destroy 뒤 같은 id request가 `actor_route_not_found`인지 확인한다. |
| TA-B1 row 없음 | implemented | `TA-B1-missing*`가 request의 `actor_route_not_found`와 send 뒤 역할 서버 evidence 부재를 검증한다. |
| TA-B2 stale location | implemented | actor-a에서 만든 ref를 저장하고 actor를 제거한 뒤 같은 id를 actor-b에서 다시 만든다. 저장한 이전 ref는 `actor_location_stale`, 다시 찾은 ref는 actor-b 성공 evidence를 남기는지 확인한다. |
| TA-B3 route not connected | 전환 필요 | 현재 source는 정식 C++ interface에 없는 `router_connections()`를 사용한다. `mesh.peer_connections()`의 public `disconnect(endpoint)`와 `connect(endpoint)`로 두 actor endpoint를 제거·복구하고, 저장한 ref의 `route_not_connected`, 복구 뒤 같은 ref와 actor-a evidence를 다시 검증해야 한다. |

`run_e2e.sh`는 Redis를 준비한 뒤 actor owner 두 개, caller, session gateway 두 개를 시작하고 모든 health를
기다린 다음 client runner를 실행한다. `E2E_START_ORDER=reverse`에서도 같은 순서 독립성을 검증한다.
현재 C++ runner는 전용 Docker Redis를 직접 시작하며, 사용자 환경에서 전달한 외부 Redis endpoint를
공유 Redis로 재사용하지 않는다.
