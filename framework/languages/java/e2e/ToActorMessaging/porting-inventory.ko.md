# ToActorMessaging Java porting inventory

## 범위

- 기준: `framework/doc/framework/common/e2e/config-9-to-actor-messaging.ko.md`
- 기준 구현: `framework/languages/dotnet/e2e/ToActorMessaging`
- 대상: `framework/languages/java/e2e/ToActorMessaging`

## 매핑

| .NET 기준 | Java 위치 | 상태 |
|---|---|---|
| `Shared/Messages.cs` | `Shared/src/main/java/.../shared/Contracts.java` | implemented |
| `Server/Actor` | `Server/Actor/src/main/java/.../actor/Program.java` | implemented |
| `Server/Caller` | `Server/Caller/src/main/java/.../caller/Program.java` | implemented |
| session gateway x2 | `Server/Session/src/main/java/.../session/Program.java`를 서로 다른 Spot·stream endpoint로 두 번 실행 | implemented |
| route mesh 없음 × session/spot 분리 | actor owner와 두 session gateway를 별도 프로세스로 실행하고, actor owner에는 relocation store를, caller/session에는 object client role을 public builder로 등록 | implemented |
| `Client` | `Client/src/main/java/.../client/Program.java` | implemented: caller response와 actor evidence를 함께 검증 |
| Track A TA-A1..TA-A4 | 실제 stream connector bind, bind 전후 push, 공개 API unbind, actor destroy를 Java client와 역할별 evidence로 검증 | implemented |
| Track B TA-B1..TA-B3 | Java client failure/success assertions | implemented: TA-B1 missing row, TA-B2 exact `ActorRef` destroy와 ActorId 재생성, TA-B3 TCP proxy 단절·복구 중 public route status와 `UNAVAILABLE`을 검증한다. |

## 검증

- `bash -n run_e2e.sh`: passed.
- `bash run_e2e.sh TA-A1`: passed in `logs/20260806-025516-3831180`.
- `bash run_e2e.sh TA-B2`: passed (`logs/20260806-033426-279679`).
- `bash run_e2e.sh TA-B3`: passed (`logs/20260806-034745-543597`).
