# Kotlin AutomaticTurnDispatch porting inventory

이번 작업에서는 Kotlin counterpart의 compile 경로에 포함되는 Delay, Shared, Client,
Server/Play, Server/Session source를 현재 public API에 맞췄다. 대표적인 contract 변경은
다음과 같다.

| 범위 | 변경 |
|---|---|
| route handler | `ZLinkRouteMessageContext` 사용 |
| spot actor handler | `ZLinkMessageContext` 사용 |
| spot/actor identity | `spotId()`, `objectGeneration()`, `ZLinkActorContext.actorId()` 사용 |
| actor factory와 join | `create(ZLinkActorContext)`, `joinSpot(...).defer()` 사용 |
| channel·spot·actor manager | `client().connect(...)`, typed `getOrCreate(...).request(...).submit()` 사용 |

검증 기준은 다음 focused compile이다. 이 명령은 Framework source composite build를 함께
컴파일하므로 stale public API가 남아 있으면 실패한다.

```text
../../gradlew --no-daemon :Client:compileKotlin :Client:compileJava :Shared:compileJava \
  :Server:Delay:compileJava :Server:Play:compileJava :Server:Session:compileJava \
  --rerun-tasks --no-build-cache
```
