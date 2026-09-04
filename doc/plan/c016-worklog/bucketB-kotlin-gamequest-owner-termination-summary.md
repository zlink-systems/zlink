# Kotlin GameQuest owner 종료 경계 실패 조사

## 결론

원인은 Kotlin GameQuest runner가 owner Mission process에 `SIGKILL`을 보낸 직후 process 종료를
기다리지 않고 client release file을 만든 race다. 수정 전
`framework/languages/java/samples/kotlin/GameQuest/run_sample.sh:228-239`는 `kill -KILL` 다음에
곧바로 `owner-terminated`를 만들었다. Client의
`Client/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/client/Program.kt:217-225`는 이 파일의
존재만 기다리므로, release는 owner process 종료나 peer-not-ready 관찰을 보장하지 않았다.

aggregate 실패에서는 그 구간에 post-termination `KillMonsterReq`가 들어왔고 GameApi의
`sendToSpot(...).submit().await()`가 아직 Ready로 보이는 기존 owner route에 접수됐다. GameApi는
접수 완료 뒤 정상 `KillMonsterRes`를 보냈으므로 `runCatching`의 `exceptionOrNull()`이 `null`이 되었고
`Program.kt:179`의 assertion이 실패했다. Client coroutine이나 stream close에서 뒤늦게 실패가
나온 경우가 아니며, 새 owner 자동 생성도 없었다.

수정은 shell runner가 `wait "$owner_pid" || true`로 killed process를 회수한 뒤에만 release file을
만들도록 한 것이다(`run_sample.sh:228-240`). PowerShell runner도 같은 경계를
`$ownerProcess.WaitForExit()`으로 맞췄다(`run_sample.ps1:288-295`). Client assertion과 Framework는
변경하지 않았다.

## 수정 전 determinism

`TMPDIR=/dev/shm/zlink-tmp-java`, `ZLINK_JAVA_STREAM_TRACE=1`,
`flock -w7200 /tmp/zlink-jvm-gate.lock` 조건에서 수정 전 Kotlin sample을 세 번 실행했다. 격리 실행은
모두 통과해 aggregate 부하에서 나타난 좁은 race를 다시 만들지는 못했다.

| 실행 | Ready owner | Framework generation | post-termination 결과 | 전체 결과 |
|---|---|---:|---|---|
| 1 | `mission-b`, replay generation 2 | object/owner generation 5 | `peer-not-ready`, reply error | 성공, `/dev/shm/zlink-tmp-java/tmp.5g1b7muLXi` |
| 2 | `mission-a`, replay generation 2 | object/owner generation 5 | `peer-not-ready`, reply error | 성공, `/dev/shm/zlink-tmp-java/tmp.7ZaZYJRNYL` |
| 3 | `mission-b`, replay generation 2 | object/owner generation 5 | `peer-not-ready`, reply error | 성공, `/dev/shm/zlink-tmp-java/tmp.zW1L3LipPo` |

세 실행 모두 마지막 `KillMonsterReq`는 GameApi에서 `received → admitted → dispatched` 뒤
`dispatch_error action=reply_error`로 끝났고, Mission node에는 post-termination `GameplayMsg`가
admit되지 않았다. 이 결과는 race가 발생하지 않은 통과 쪽 trace다.

## 실패와 통과 trace 비교

### 기존 aggregate 실패

- `zlink-work/c016/logs/gate-final-java-aggregate.log:953-954`에서 `player-alice`의 replay owner는
  `mission-b`, sample replay generation 2였다. 이 실행에는 raw stream trace가 없어 Framework의
  object/owner generation 숫자는 기록되지 않았다.
- 같은 로그 `:741-745`에서 post-termination request는 `api-a`가 처리한 `corr=9`,
  `flow=01a06e13-e136-719b-af1a-8bd9dee0a75b`다. Message flow는
  `received → admitted → dispatched → replied`를 모두 `outcome=succeeded`로 기록했고 그 사이
  `gamequest-api event-routed player=player-alice`가 출력됐다.
- 죽이도록 선택한 owner는 `mission-b`의 기존 replay owner였고 replacement marker는 없다. Mission
  로그에는 이 마지막 `GameplayMsg`의 admitted/dispatched evidence가 없다. 따라서 확인되는 완료는
  GameApi의 one-way Spot send 접수이며 Mission domain handler 완료가 아니다.
- Client stack은 reply future가 정상 완료된 뒤 `Program.kt:179`에서 실패했다. 그러므로 stream close에
  숨어 있던 예외를 `runCatching`이 놓친 경우가 아니다.

### 수정 후 대표 통과

`/dev/shm/zlink-tmp-java/tmp.XoA5T5Kogx/logs/api-a.log`에서 직전 owner request reply는
`targetNode=gamequest-mission-mission-a objectGeneration=5 ownerGeneration=5`다(`:242`). 마지막
`KillMonsterReq`는 같은 파일 `:244-246`에서 `api-a`의
`flow=01a06e28-3d9d-7ed1-b36c-097f287dcc0f`로 received/admitted/dispatched됐다. 이후 흐름은 다음과
같다.

1. `api-a`가 generation 5 owner node인 `mission-a`로 Spot send를 제출했다(`:247`).
2. route가 `reason=peer-not-ready`로 거부됐다(`:248-249`).
3. 살아 있는 `api-a`가 `gamequest-owner unavailable player=player-alice`를 출력했다(`:250`).
4. STREAM handler가 `dispatch_error ... action=reply_error`를 남겼다(`:251`). 성공 reply나 새 owner
   generation은 없었다.

다른 두 수정 후 실행도 같은 transition이며 owner node만 배치 결과에 따라 달랐다.

## 원인과 Java 계약 비교

Kotlin GameApi는
`Server/GameApi/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/gameapi/Program.kt:280-289`에서
one-way `sendToSpot` 접수 완료 뒤 `KillMonsterRes`를 보낸다. 이 자체는 기존 sample 동작이며 이번
변경 대상이 아니다. owner process가 실제로 종료되기 전에 client를 풀면 아직 Ready인 outbound
route가 send를 접수할 수 있다는 점이 runner race를 사용자-visible 성공 reply로 만들었다.

Java client도 release file 뒤 gameplay call이 `Unavailable`이어야 한다는 같은 assertion을 유지한다
(`samples/java/GameQuest/Client/.../GameQuestClientScenario.java:231-245`). 차이는 Java shell runner가
정확한 `gamequest-owner-ready` evidence로 owner process를 고른 뒤 `kill -9`, `wait`, release 순서를
사용한다는 점이다(`samples/java/GameQuest/run_sample.sh:317-330`). Kotlin client의 기대는 Java
계약과 모순되지 않으므로 client 의미나 assertion을 바꾸지 않고 runner의 종료 경계만 Java와 맞췄다.

공통 sample 계약도 다음을 요구한다.

- `framework/doc/framework/common/sample/event/gamequest.ko.md:51`: Ready owner 장애 뒤 operation은
  `Unavailable`이다.
- 같은 문서 `:138-142`, `:342-345`: Instance intent는 owner 장애 뒤 실패한 operation을 자동
  replacement node로 재제출하지 않는다.
- 같은 문서 `:396-397`: 강제 종료 뒤 다음 gameplay call의 `Unavailable`과 replacement handler 0회를
  client self-check가 확인한다.
- 같은 문서 `:474-477`: runner가 owner-ready 표시로 Mission process를 특정해 종료하고, 그 뒤에
  client를 진행시키는 단계 제어가 필요하다.

계약 문서는 read-only로 확인했으며 수정하지 않았다.

## 수정과 regression

- `framework/languages/java/samples/kotlin/GameQuest/run_sample.sh`: killed owner PID를 `wait`한 뒤
  cleanup 목록에서 빼고 release file을 만든다.
- `framework/languages/java/samples/kotlin/GameQuest/run_sample.ps1`: 선택한 owner process를
  `WaitForExit()`한 뒤 release file을 만든다.
- `Program.kt:175-179`의 `ensure(unavailable != null)`는 그대로다. assertion을 낮추지 않았다.
- Framework 변경이 아니므로 `zlink-framework-core` regression test를 추가하거나 전체 core test를
  실행하지 않았다. Process 종료와 runner-client 단계 제어는 Kotlin GameQuest process sample 3회로
  회귀 검증했다.

## 검증 결과

모든 Gradle/sample 실행은 `/tmp/zlink-jvm-gate.lock`으로 직렬화했다. 최종 실행은 directory가 아닌
파일 값
`ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so`를
사용했고 Core/local package를 다시 만들지 않았다.

| 검증 | 결과 | 보존 경로 |
|---|---|---|
| Kotlin GameQuest 수정 후 1 | 성공, completion/evidence/placement marker 모두 확인 | `/dev/shm/zlink-tmp-java/tmp.XoA5T5Kogx` |
| Kotlin GameQuest 수정 후 2 | 성공, completion/evidence/placement marker 모두 확인 | `/dev/shm/zlink-tmp-java/tmp.K3BarHqb54` |
| Kotlin GameQuest 수정 후 3 | 성공, completion/evidence/placement marker 모두 확인 | `/dev/shm/zlink-tmp-java/tmp.sLydyIkndG` |
| Java GameQuest parity | 성공, completion/evidence/placement marker 모두 확인 | `/dev/shm/zlink-tmp-java/tmp.6nH1qCiP0g` |
| shell syntax, diff whitespace, PowerShell parser | 성공 | 해당 없음 |

최종 3회와 별도로 Kotlin 한 번은 두 Mission process가 native library를 찾지 못해 topology 전에
실패했다(`/dev/shm/zlink-tmp-java/tmp.fKVUXL0YZy`). file-valued `ZLINK_LIBRARY_PATH`를 지정한 재실행은
성공했으므로 scenario 결과에서 제외했다. Java parity의 첫 trace 실행은 dedicated owner
`JoinSessionReq`가 20초 timeout으로 실패했다(`/dev/shm/zlink-tmp-java/tmp.kyvAbz0GUv`). auxiliary
stream trace를 끈 재실행은 성공했다.

## BLOCKERS

- 이번 Kotlin runner 수정과 요청된 성공 검증에는 blocker가 없다.
- 작업 중 다른 job의 변경으로
  `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaStreamSocket.java`와
  대응 test가 dirty 상태가 됐다. 이 job에서는 두 파일을 읽거나 수정하지 않았고 core test도 실행하지
  않았다.
- 요청된 `framework/languages/java/AGENTS.md`는 repository에 존재하지 않는다. 적용 가능한 root
  `AGENTS.md`, `framework/AGENTS.md`, `doc/AGENTS.md`를 따랐다.
