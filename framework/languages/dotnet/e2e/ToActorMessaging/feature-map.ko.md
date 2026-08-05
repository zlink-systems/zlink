# ToActorMessaging feature-map

이 스위트는 공통 config-9 `to-actor messaging` 문서의 .NET 구현 위치다.

| ID | 상태 | .NET 구현 |
|----|------|-----------|
| TA-A1 | 구현 | session-a connector가 actor를 bind하고 Before/After bound push를 받는 사이 caller 서버의 no-bind send/request를 실행한다. 연결만 하고 bind하지 않은 session-b connector에는 push가 도달하지 않음을 함께 확인한다. |
| TA-A2 | 구현 | `TA-A2-unbound-*`가 session binding을 만들지 않은 actor에 대해 caller 서버의 no-bind send/request와 owner mailbox evidence를 검증한다. |
| TA-A3 | 구현 | actor를 먼저 만든 뒤 bind 전 no-bind send/request를 실행하고, session-b connector bind 뒤 다시 no-bind send/request와 `LateBindNotify` bound push를 검증한다. |
| TA-A4 | 구현 | session-a connector bind와 push를 확인한 뒤 connector를 종료한다. actor가 유지되는 동안 no-bind send/request가 성공하고, 명시적 actor destroy 뒤 caller가 보관한 동일 ref request가 `ActorRouteNotFound`로 끝나는지 검증한다. |
| TA-B1 | 구현 | live actor와 일치하지 않는 public `ActorRef`로 request를 보내 caller의 `ActorRouteNotFound`를 검증한다. one-way send는 로컬 submit 뒤 handler·lifecycle evidence와 location row가 생기지 않아 auto-create나 메시지 보관이 없음을 검증한다. |
| TA-B2 | 구현 | caller 서버는 framework `ActorLocationStale` kind를 그대로 JSON으로 반환한다. supervisor는 stale descriptor 조작 뒤 같은 endpoint로 검증한다. |
| TA-B3 | 구현 | `logs/20260720-035309-2020196`: 현재 actor row를 공유한 caller가 수동 route를 끊었을 때 handler 비실행과 route 실패를 확인하고, 같은 lifetime route를 다시 연결한 뒤 같은 `ActorRef` request와 actor-a handler evidence가 성공했다. |

`run_e2e.sh`는 실행마다 전용 Docker Redis container, actor-a/actor-b 서버, session-a/session-b gateway,
caller 서버와 client runner를
모두 시작한다. 이미 실행 중인 Redis나 host Redis endpoint를 재사용하지 않는다. Docker를 사용할 수 없으면
명확한 오류를 출력하고 중단한다.
