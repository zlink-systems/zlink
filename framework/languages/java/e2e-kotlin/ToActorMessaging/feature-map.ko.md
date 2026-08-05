# ToActorMessaging feature-map

이 디렉토리는 공통 config-9 `to-actor messaging` 문서의 Kotlin 구현 범위를 기록한다.

| 공통 항목 | Kotlin 구현 |
|-----------|-------------|
| TA-A1 bind된 actor send/request | `Client/src/main/java/.../Program.java`가 actor를 준비한 뒤 caller 서버의 `/send`, `/request`를 호출한다. Caller role은 one-way call의 public `submit()`과 request의 Kotlin public `awaitReply` extension을 사용한다. |
| TA-A2 bind 안 된 actor send/request | `TA-A2-unbound-*`가 session binding 없이 서버 측 caller에서 actor mailbox 전달과 reply를 확인한다. |
| TA-A3 no-bind 전달 뒤 이후 bind | 차단. 현재 runner는 생성 전 fail-fast와 생성 뒤 mailbox 전달만 확인하며, 실제 STREAM session을 이후 bind한 뒤 push까지 검증하지 않는다. session gateway role과 bind·push evidence를 추가해야 한다. |
| TA-A4 unbind/disconnect 후 | 차단. 현재 runner는 처음부터 session binding이 없는 actor row만 사용한다. 실제 session을 먼저 bind하고 unbind 또는 disconnect한 뒤 Actor 유지와 send/request를 검증해야 한다. |
| TA-B1 row 없음 | 차단. shared Java runtime의 missing route 분류가 공통 계약과 다르므로 현재 `TA-B1-missing*` 결과를 완료 증거로 사용하지 않는다. runtime 오류 분류를 수정한 뒤 actor handler 미도달과 정확한 public error를 함께 검증해야 한다. |
| TA-B2 stale location | Actor owner 서버가 public location store API로 stale actor row를 만들고, client가 caller 서버의 `ACTOR_LOCATION_STALE` 반환, fault handler evidence 없음, 복구 뒤 request 성공을 확인한다. |
| TA-B3 route not connected | Actor owner 서버가 public location store API로 연결되지 않은 routing id를 가진 actor row를 만들고, client가 caller 서버의 `ROUTE_NOT_CONNECTED` 반환, fault handler evidence 없음, 복구 뒤 request 성공을 확인한다. |

`run_e2e.sh`는 Redis, actor owner 서버, Kotlin caller 서버, client runner를 모두 시작한다. Runner는
실행마다 전용 Docker Redis container를 만들며, container를 만들지 못하면 즉시 실행을 실패로
종료한다.

2026-07-07 checkout의 `nice -n 10 timeout 420s ./run_e2e.sh`는 기존 runner 범위에서 통과했지만,
TA-A3·TA-A4가 공통 시나리오를 구현하지 않아 Config 9 완료 증거로 사용하지 않는다. 당시 로그는
`logs/20260707-172116-2831364/client.log`이며 기존 최종 marker는
`to-actor-messaging e2e result=passed`다. 새 runner는 두 STREAM lifecycle을 포함한 뒤에만 같은 최종
marker를 출력해야 한다.

## 검증 메모

fault control은 Redis 키를 직접 조작하지 않는다. Actor owner 서버가 framework public location store
API로 actor row를 갱신하고, Kotlin caller 서버는 public `ZLinkActorClient`와 coroutine await extension
으로 send/request를 실행한다. Client runner는 fault scenario 이름이 actor evidence에 남지 않는지 확인해
stale descriptor와 disconnected route 상태에서 actor handler가 실행되지 않았음을 검증한다.
