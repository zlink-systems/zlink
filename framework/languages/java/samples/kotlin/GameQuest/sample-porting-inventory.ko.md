# Kotlin GameQuest Sample .NET 기준 포팅 Inventory

이 문서는 Kotlin `GameQuest` 샘플을 `.NET` 기준 구현과 공통 event sample 문서에 맞춰 대조한 결과다.
`gap`이나 `partial`이 남아 있으면 이 샘플은 완료로 보지 않는다.

## 기준 파일 대응

| 기준 | Kotlin 대응 | 분류 | 상태 | 비고 |
|------|-------------|------|------|------|
| `.NET: GameQuest.csproj` | `build.gradle.kts` | build | done | Kotlin 루트는 하위 role project를 묶는다. |
| `.NET: README.ko.md` | `README.md` | docs | done | Kotlin 역할, Redis 준비 방식, 성공 marker를 설명한다. |
| `.NET: run_sample.sh` | `run_sample.sh` | runner | done | GameApi 2개, QuestMission 2개, Client와 실행별 전용 Docker Redis를 실행한다. |
| `.NET: run_sample.ps1` | `run_sample.ps1` | runner | done | Windows runner도 같은 role 구성과 Redis endpoint/key prefix 계약을 사용한다. |
| `.NET: Client/GameQuest.Client.csproj` | `Client/build.gradle.kts` | build | done | Client role project. |
| `.NET: Client/Program.cs` | `Client/src/main/kotlin/.../client/Program.kt` | client-entry | done | 두 stream connector를 만들고 self-check scenario를 실행한다. |
| `.NET: Client/GameQuestClientScenario.cs` | `Client/src/main/kotlin/.../client/Program.kt` | client-scenario | done | session join, quest progress push, completion push, idempotency, projection rebuild, owner 재활성, sync/reconcile을 검증한다. |
| `.NET: Client/Configuration/SampleConfiguration.cs` | `Server/Configuration/src/main/kotlin/.../Configuration.kt` | configuration | done | endpoint, Redis, timing, marker 설정을 공유한다. |
| `.NET: Shared/GameQuest.Shared.csproj` | `Shared/build.gradle.kts` | build | done | Shared project. |
| `.NET: Shared/Messages.cs` | `Shared/src/main/kotlin/.../shared/contracts/Messages.kt` | shared-contract | done | gameplay action, quest progress, notification, self-check DTO를 둔다. |
| `.NET: Server/Configuration/GameQuest.Server.Configuration.csproj` | `Server/Configuration/build.gradle.kts` | build | done | 서버 공통 설정 project. |
| `.NET: Server/Configuration/SampleConfiguration.cs` | `Server/Configuration/src/main/kotlin/.../Configuration.kt` | server-config | done | role endpoint, channel, Redis location store, sample state key, marker를 모은다. |
| `.NET: Server/Configuration/RedisJsonStore.cs` | `Server/Configuration/src/main/kotlin/.../Configuration.kt` | external-adapter | done | Kotlin은 `RedisSampleStore`가 projection, event, gameplay fact를 sample store 뒤에 둔다. |
| `.NET: Server/Configuration/SampleFlowLog.cs` | `Server/GameApi`, `Server/QuestMission`, `GAMEQUEST_LOG_DIR` | evidence | done | role별 flow log를 남기고 runner가 sample client/server marker를 확인한다. |
| `.NET: Server/GameApi/GameQuest.GameApi.csproj` | `Server/GameApi/build.gradle.kts` | build | done | GameApi role project. |
| `.NET: Server/GameApi/Program.cs` | `Server/GameApi/src/main/kotlin/.../gameapi/Program.kt` | server-entry | done | GameApi stream role과 self-check HTTP endpoint를 실행한다. |
| `.NET: Server/GameApi/Session/GameQuestSession.cs` | `Server/GameApi/src/main/kotlin/.../gameapi/Program.kt` | stream-session | done | stream session이 `PlayerId` Session Actor를 만들고 Entry Spot에 배치한 뒤 연결에 bind한다. |
| `.NET: Server/GameApi/Application/GameplayActionService.cs` | `GameQuestSession`, `GameQuestStore` | application | done | gameplay action을 event envelope로 만들고 Instance Spot에 one-way로 보낸다. 결과는 direct Actor send와 bound session push로 돌아온다. |
| `.NET: Server/GameApi/Domain/GameplayDomain.cs` | `GameQuestStore`, `RedisSampleStore` | domain-store | done | gameplay fact snapshot과 projection merge를 sample store 뒤에 둔다. |
| `.NET: Server/QuestMission/GameQuest.QuestMission.csproj` | `Server/QuestMission/build.gradle.kts` | build | done | QuestMission role project. |
| `.NET: Server/QuestMission/Program.cs` | `Server/QuestMission/src/main/kotlin/.../questmission/Program.kt` | server-entry | done | `PlayerQuestSpot` Instance Spot owner와 self-check HTTP endpoint를 실행한다. |
| `.NET: Server/QuestMission/Application/QuestEventProcessor.cs` | `QuestStore.apply()` | application | done | event idempotency, progress update, reward grant, notification 생성 책임을 갖는다. |
| `common: PlayerId direct owner routing` | `SampleNames.PlayerQuestMesh`, `PlayerQuestSpotType` | routing | done | 호출자는 `PlayerId`를 SpotId로 사용하고 Instance marker를 붙인다. Framework가 최초 owner를 선택한다. |
| `.NET: Server/QuestMission/Domain/QuestDomain.cs` | `QuestDomain` in `Program.kt` | domain | done | quest 조건 평가와 progress/reward event 생성을 맡는다. |

## 공통 요구 대응

| 기준 | Kotlin 대응 | 분류 | 상태 | 비고 |
|------|-------------|------|------|------|
| common: GameApi 2개, QuestMission 2개 실행 | `run_sample.sh` | runner | done | 실제 process 경계로 role을 실행하고 공통 Redis location store로 서로를 찾는다. |
| common: client는 stream session으로 gameplay action과 push를 처리 | `Client/Program.kt`, `GameQuestSession` | validation | done | `JoinSessionReq`, gameplay action request, progress/completion notify를 같은 stream에서 검증한다. |
| common: player별 owner에서 quest event 직렬 처리 | `PlayerQuestSpot`, QuestMission Spot handlers | runtime-flow | done | 같은 `PlayerId`의 request는 같은 Instance Spot queue에서 직렬 처리한다. |
| common: quest event store replay와 projection rebuild | `QuestStore`, `RedisSampleStore`, rebuild self-check endpoint | validation | done | projection 삭제 뒤 rebuild request로 `HerbGathering` reward projection을 복구한다. |
| common: gameplay fact 기반 reset/reconcile | `SyncQuestProgressReq`, `/self-check/gameplay/kill-without-publish/*` | validation | done | owner messaging 없이 누락된 gameplay fact를 기록한 뒤 sync/reconcile로 projection을 보정한다. |
| common: reward idempotency | duplicate `KillMonsterReq` check | validation | done | 같은 idempotency key가 같은 event id를 반환하는지 client가 확인한다. |
| common: client reconnect 후 조회 복원 | player-b offline event 뒤 join | validation | done | offline herb event 뒤 player-b join이 active quest progress를 복원하는지 확인한다. |
| common: bound session push | `GameQuestPlayerActor`, `QuestProcessingActorHandler` | validation | done | QuestMission이 `PlayerId` Actor에 direct send하고 Actor가 현재 bound session으로 progress/completion을 push한다. |
| common: Redis-backed location store | `SampleLocationStore.create()` | runtime-config | done | GameApi와 QuestMission role이 `ZLinkRedisLocationStore` bean을 등록한다. |
| common: 실행별 전용 Redis 사용 | `run_sample.sh`, `run_sample.ps1` | runner | done | runner가 pinned image로 전용 Docker Redis를 만들고 외부 endpoint를 재사용하지 않는다. |
| common: Docker Redis는 runner 책임 | `run_sample.sh`, `run_sample.ps1` | runner | done | 애플리케이션은 runner가 만든 endpoint만 받고, runner는 자신이 만든 container id만 정리한다. |
| common: 실행별 Redis key prefix 사용 | `run_sample.sh`, `Configuration.kt` | runner | done | `GAMEQUEST_REDIS_KEY_PREFIX`가 없으면 실행별 prefix를 만든다. |
| common: server evidence check | `GameQuestClientScenario.waitForServerAssertion()` | validation | done | server assertion이 통과하면 `gamequest-server-evidence=completed` marker를 출력한다. |
| common: Domain은 framework 타입을 모름 | `QuestDomain` | design | done | quest 조건 평가와 progress event 생성은 framework context 없이 동작한다. |

## 남은 gap

- Java runner에서 같은 계약의 첫 remote Instance Spot 활성화가 거절된다. Kotlin은 compile과
  topology 구성을 완료했으며 이 runtime activation gap이 닫힌 뒤 실제 scenario를 다시 검증한다.

## 최근 검증 메모

- 2026-07-30 JVM samples 전체 `classes` 통과.
- 증거 파일: `build/sample-logs/client.log`, `logs/flow-api-a.log`, `logs/flow-api-b.log`,
  `logs/flow-mission-a.log`, `logs/flow-mission-b.log`.
- runner cleanup은 직접 기록한 process와 runner가 만든 Redis container만 정리한다. role 이름 pattern으로
  기존 process를 찾아 종료하지 않는다.
