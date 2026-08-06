# Java AutomaticTurnDispatch feature map

기준 계약은 `framework/doc/framework/common/e2e/config-8-execution-turn.ko.md`와 현재 Java public
API 문서다. 이 변경에서는 fixture의 stale 호출을 현재 API로 교체하고, `TD-*` selector가 실제
message, Spot handler, actor/timer/worker 경로를 호출하도록 구성했다. 실행 evidence는 아래 blocker가
해소된 뒤 focused runner에서 확정해야 한다.

| selector | 현재 상태 | 근거 또는 남은 조건 |
|---|---|---|
| `TD-A3`, `TD-B3` | code-ready | counter reset/read와 8개 async/yield continuation을 실제 Spot 상태로 검증한다. |
| `TD-A5`, `TD-B4` | code-ready | timer tick handler가 public channel `submit/yield`와 다음 tick을 실제로 검증한다. |
| `TD-C1`~`TD-C5` | code-ready | public `runIoWorker`/`runCpuWorker`와 batch request를 실제 handler에서 호출한다. |
| `TD-D1`~`TD-D4`, `TD-D6` | code-ready | 기존 actor/timer/timeout fixture를 현재 actor, timer, request surface로 연결한다. actor admission 완료를 다음 request 전에 확인하는 runtime evidence는 아래 contract gap의 영향을 받는다. |
| `TD-E1`, `TD-E2` | code-ready | user Spot join과 반대 방향 join을 실제 actor join request로 실행한다. |
| `TD-E2A` | contract-gap | 현재 `ZLinkActorJoinCall.defer()`는 결과/실패 stage를 반환하지 않는다. handler 실패 후 deferred join barrier를 관찰하는 공개 경로가 없어 같은 의미를 보장할 수 없다. |
| `TD-F1`~`TD-F6` | code-ready | remote Spot, route bridge, session relay, timeout/cancel 경로를 기존 public fixture로 실행한다. cycle 자체의 독립 evidence는 후속 보강 대상이다. |
| `TD-F5A` | blocked | source host의 public drain seal 뒤 pending await는 종료되지만, 같은 session에서 시작한 새 request가 Java runtime의 정식 `ShuttingDown`이 아니라 `TimeoutException`으로 끝난다. 실행 로그 `logs/20260806-045139-1930524/`의 `session-flow.log`와 `client-td-f5a-probe.stderr.log`가 이 결과를 보인다. 이 error mapping을 runner에서 성공으로 분류하지 않는다. |
| `TD-G1` | partial | Java public terminator surface compile check와 async scenario selector는 있으나 cross-language parity evidence는 Kotlin compile blocker 이후 확정한다. |
| `TD-A1`, `TD-A2`, `TD-A4`, `TD-B1`, `TD-B2`, `TD-D5` | partial | 기존 selector 또는 public method-reference contract check가 있다. `TD-D5`는 unsupported context를 런타임에 만들 공개 API가 없어 compile-time contract check로 제한된다. |

## 검증 조건

`framework/languages/java/e2e/AutomaticTurnDispatch`에서 `../../gradlew compileJava --no-daemon
--console=plain`이 통과했다. `bash -n run_e2e.sh`와 `git diff --check`도 통과한다.

focused `run_e2e.sh TD-A3`, `TD-A5`, `TD-C3`는 각각 실제 server topology와 evidence를 사용해
`result=passed`를 확인했다. `run_e2e.sh all`은 ATD-A1~A4와 ATD-B1까지 통과한 뒤 ATD-B2에서
중단됐다. `ActorJoinCall.defer()`가 `void`를 반환하므로 deferred admission 완료를 public API로
기다릴 수 없는 것이 원인이다. 이 조건을 감추기 위해 sleep, reflection, raw frame 또는 internal
API를 추가하지 않았다.

Kotlin counterpart는 Delay fixture 일부를 current channel API로 갱신했지만, Play/Shared 전반에
삭제된 context, generic join result, old factory/getOrCreate/enableClient 호출이 남아 있어 별도
범위로 남긴다.
