# ToActorMessaging feature-map

이 스위트는 공통 config-9 `to-actor messaging`의 C++ 구현 위치다.

- public 표면: `zlink::framework::actor_client_t`
- send 터미널: `send_to_actor(...).async()`
- request 터미널: `request_to_actor(...).async<TReply>()`
- 실패 분류: `actor_route_not_found`, `unavailable`

최근 TA-B3 runner 통과 proof는 `logs/20260806-192003-1491436`에 남아 있다. 앞선 실행에서 복구
상태가 제한 시간 안에 `ready`로 수렴하지 않은 실패도 확인했으므로, 단일 실행 결과만으로 안정성을
판정하지 않고 bounded public status와 실제 route socket을 함께 검증한다.

| 공통 항목 | 상태 | C++ 구현 |
|-----------|------|----------|
| TA-A1 bind된 actor send/request | implemented | `session-a` connector가 bind 전후 push를 받고, caller의 send/request 뒤에도 bind evidence가 한 건으로 유지되는지 확인한다. |
| TA-A2 bind 안 된 actor send/request | implemented | 두 session gateway에 bind evidence가 없는 상태에서 actor mailbox send와 request reply를 확인한다. |
| TA-A3 no-bind 전달 뒤 이후 bind | implemented | bind 전 send/request 뒤 `session-b`에 bind하고, bind 후 send/request와 `LateBindNotify` push를 확인한다. |
| TA-A4 unbind/disconnect 후 | implemented | connector disconnect evidence 뒤 같은 actor 호출이 성공하고, 명시적 destroy 뒤 같은 id request가 `actor_route_not_found`인지 확인한다. |
| TA-B1 row 없음 | implemented | `TA-B1-missing*`가 send/request 모두 `actor_route_not_found`를 반환하고 역할 서버 evidence를 만들지 않는지 검증한다. |
| TA-B2 stale location | implemented | Actor process의 ensure fixture는 `actor_manager_t` public creation 경로를 사용한다. destroy callback은 authority와 stateful reservation을 정리하고, committed creation reservation도 authority delete와 같은 CAS에서 제거한다. 최신 `logs/20260806-153039-728806`에서 재생성, ID-only message, 이전 `ActorRef` lifecycle `invalid_operation`, 새 Actor evidence를 모두 확인했다. |
| TA-B3 route not connected | implemented | `mesh_peer_connections_t`의 disconnect가 topology와 liveness에서 같은 connection identity를 제거하고, actor request의 route-unavailable terminal을 `Unavailable`로 변환한다. Runner는 caller process가 소유한 실제 actor-b 연결을 확인한 뒤 public status `not_ready`, 단절 중 handler 미실행과 `Unavailable`, 복구 후 `ready`와 같은 ActorRef request 성공을 검증한다. HTTP 상태 polling과 transport probe는 모두 bounded timeout을 사용한다. |

`run_e2e.sh`는 Redis를 준비한 뒤 actor owner 두 개, caller, session gateway 두 개를 시작하고 모든 health를
기다린 다음 client runner를 실행한다. `E2E_START_ORDER=reverse`에서도 같은 순서 독립성을 검증한다.
현재 C++ runner는 전용 Docker Redis를 직접 시작하며, 사용자 환경에서 전달한 외부 Redis endpoint를
공유 Redis로 재사용하지 않는다.
