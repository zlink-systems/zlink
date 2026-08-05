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
| route mesh 없음 × session/spot 분리 | actor owner와 두 session gateway를 별도 프로세스로 실행하고, 세 역할 모두 SpotMesh와 location store만 등록 | implemented |
| `Client` | `Client/src/main/java/.../client/Program.java` | implemented: caller response와 actor evidence를 함께 검증 |
| Track A TA-A1..TA-A4 | 실제 stream connector bind, bind 전후 push, 공개 API unbind, actor destroy를 Java client와 역할별 evidence로 검증 | implemented |
| Track B TA-B1..TA-B3 | Java client failure/success assertions | partial: TA-B2/TA-B3는 public `ActorRef` 직접 호출과 actor evidence 부재를 검증한다. TA-B1은 E2E-JV-15의 runtime error-kind blocker로 open이다. |

## 검증

- `./run_e2e.sh TA-A1` .. `./run_e2e.sh TA-A4`
- `logs/20260715-030607-1313313`, `logs/20260715-030622-1314553`,
  `logs/20260715-030635-1315721`, `logs/20260715-030651-1317573`:
  각 selector에서 `to-actor-messaging e2e result=passed`.
- `logs/20260715-030710-1319569`, `logs/20260715-030725-1320790`:
  Track B 회귀 TA-B2·TA-B3 통과.
