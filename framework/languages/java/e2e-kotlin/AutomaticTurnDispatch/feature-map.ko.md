# Kotlin AutomaticTurnDispatch feature map

Kotlin counterpart는 Java fixture와 disjoint하게 끝낼 수 있는 Delay fixture 두 파일만 현재
channel API로 갱신했다. 나머지 Kotlin AutomaticTurnDispatch source는 현재 public API와 아직 맞지
않으므로 완료로 표시하지 않는다.

| 범위 | 상태 | 근거 또는 blocker |
|---|---|---|
| `Server/Delay/DelayApplication.java` | partial | `server().listen(...).addRequestHandler(...)`로 갱신했다. |
| `Server/Delay/DelayHandler.java` | partial | 삭제된 request context 대신 현재 message context를 사용한다. |
| `Shared`, `Client`, `Server/Play`, `Server/Session` | blocked | 삭제된 `ZLinkRouteRequestContext`/`ZLinkSpotActorRequestContext`, old `spotRid()`, generic `ZLinkActorJoinResult`, old factory/getOrCreate/channel 호출이 남아 compile이 중단된다. |

Kotlin focused compile에서 약 70개의 stale public API 오류가 확인됐다. Java 범위를 침범하거나
reflection/raw/internal workaround를 추가하지 않고 이 작업에서는 Kotlin 전체 포팅을 완료했다고
주장하지 않는다.
