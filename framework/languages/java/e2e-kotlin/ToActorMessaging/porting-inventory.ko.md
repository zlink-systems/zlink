# ToActorMessaging Kotlin porting inventory

## 범위

- 기준: `framework/doc/framework/common/e2e/config-9-to-actor-messaging.ko.md`
- 기준 구현: `framework/languages/dotnet/e2e/ToActorMessaging`
- 참고 구현: `framework/languages/java/e2e/ToActorMessaging`
- 대상: `framework/languages/java/e2e-kotlin/ToActorMessaging`

## 매핑

| .NET/Java 기준 | Kotlin 위치 | 상태 |
|---|---|---|
| `Shared/Messages.cs`, Java `Shared/Contracts.java` | `Shared/src/main/java/.../shared/Contracts.java` | implemented |
| actor owner 서버 | `Server/Actor/src/main/java/.../actor/Program.java` | implemented |
| caller 서버 | `Server/Caller/src/main/kotlin/.../caller/Program.kt` | implemented |
| client runner | `Client/src/main/java/.../client/Program.java` | implemented |
| Track A TA-A1..TA-A4 | client scenario assertions + actor owner evidence + Kotlin caller endpoint | implemented |
| Track B TA-B1 | missing actor send/request assertions | implemented |
| Track B TA-B2 | stale actor row 강제, `ACTOR_LOCATION_STALE` 반환, handler 미실행, 복구 뒤 request 성공 | implemented |
| Track B TA-B3 | route disconnected row 강제, `ROUTE_NOT_CONNECTED` 반환, handler 미실행, 복구 뒤 request 성공 | implemented |
| `run_e2e.sh` | Redis 준비, actor/caller/client 실행, pass marker 확인 | implemented |

## Kotlin public surface 사용

Caller role은 Java 공용 framework의 `ZLinkActorClient`를 Spring bean으로 받고,
one-way send는 public call builder의 `submit()`으로 제출하고 request는 Kotlin public coroutine
extension인 `awaitReply`로 응답을 기다린다. Packet 이름과 timeout은 기존 public call builder에서
지정한다. Java internal package,
raw frame 조작, test-only adapter는 사용하지 않는다.

## fault 검증 방식

Actor owner 서버는 framework의 public Redis location store API로 actor location row를 갱신한다.
TA-B2는 live owner lease가 있는 actor row의 actor ref generation을 앞당겨 stale 상태를 만들고,
caller 서버가 `ACTOR_LOCATION_STALE`을 반환하는지 확인한다. TA-B3는 live owner lease는 유지하되
actor row의 routing id를 연결되지 않은 값으로 바꾸고, caller 서버가 `ROUTE_NOT_CONNECTED`를
반환하는지 확인한다. 두 경우 모두 fault scenario 이름으로 actor handler evidence가 생기지 않는지도
검증한 뒤, 저장해 둔 live row를 더 높은 generation으로 복구하고 같은 actor에 대한 request/reply가
다시 성공하는지 확인한다.

## 검증

- `nice -n 10 timeout 420s ./run_e2e.sh`

위 명령은 2026-07-07 현재 checkout에서 통과했다. 최신 증거는
`logs/20260707-172116-2831364/client.log`이며, runner는
`to-actor-messaging e2e result=passed`를 출력했다.
