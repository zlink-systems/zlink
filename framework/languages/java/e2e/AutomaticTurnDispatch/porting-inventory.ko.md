# Java AutomaticTurnDispatch porting inventory

구형 fixture를 현재 Java public surface에 맞춘 변경 목록이다.

| 영역 | 현재 public API 사용 |
|---|---|
| actor factory | `ZLinkActorFactory.create(ZLinkActorContext)` |
| actor join | `joinSpot(String, ...)`과 `timeout(...).defer()`; 현재 join result는 non-generic이다. |
| actor create | `getOrCreate(String actorId, String type).request(...).submit()` |
| spot create | `getOrCreate(String spotId, String spotType).request(...).submit()` |
| routing | `requestToSpot(String, ...)`/`sendToSpot(String, ...)`; `SpotHandle`을 route argument로 넘기지 않는다. |
| actor binding | `ActorBinding.generation()`과 현재 `ActorRef` four-field constructor를 사용한다. |
| channel | delay server는 `server().listen(...)`, client는 `client().connect(...)`를 사용한다. |
| terminator | request와 worker의 public `submit/yield`, join의 public `defer` surface를 사용한다. |
| worker scenarios | `runIoWorker`와 `runCpuWorker` task는 `CompletionStage`를 반환하며 caller code에서 raw/internal path를 사용하지 않는다. |

Java 대상 fixture compile은 다음 명령으로 확인했다.

```text
cd framework/languages/java/e2e/AutomaticTurnDispatch
../../gradlew compileJava --no-daemon --console=plain
```

`run_e2e.sh`에는 `TD-A3/A5`, `TD-B3/B4`, `TD-C1..C5`, `TD-D1..D6`, `TD-E1/E2/E2A`,
`TD-F1..F6`, `TD-G1` selector와 `all` 집계가 들어갔다. `TD-A3`, `TD-A5`, `TD-C3` focused
runner는 실제 `result=passed`를 확인했다. `all`은 ATD-B2에서 deferred actor admission 완료를
기다릴 public API가 없어 중단된다. 현재 `ZLinkActorJoinCall`은 `timeout(Duration)`과 `void
defer()`만 제공하므로, 그 완료를 대신하기 위해 fixture에 timing workaround나 internal 접근을
추가하지 않았다.

`TD-E2A`와 `TD-D5`는 현재 public contract가 failure barrier 또는 unsupported-context runtime
검증을 제공하지 않으므로 각각 contract gap으로 기록한다. 이 gap을 메우기 위해 reflection, raw
frame, internal API, test-only adapter 또는 새 public API를 추가하지 않았다.
