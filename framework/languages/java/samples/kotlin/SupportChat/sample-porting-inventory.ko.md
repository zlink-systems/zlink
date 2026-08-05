# SupportChat Kotlin sample porting inventory

## 기준

이 inventory는 공통 SupportChat sample 문서와 `.NET` SupportChat 구현을 기준으로 Kotlin
샘플에 남은 작업을 추적한다. Java SupportChat 샘플은 같은 JVM framework 위에서 이미 닫힌
대응 구현이므로 Kotlin 포팅 시 구조와 runner evidence를 비교하는 참고 자료로 사용한다.

기준 문서와 구현:

- `framework/doc/framework/common/sample/supportchat/README.ko.md`
- `framework/languages/dotnet/samples/SupportChat/`
- `framework/languages/java/samples/java/SupportChat/`

## 포팅 상태

| 기준 | Kotlin 대응 | 분류 | 상태 | 비고 |
|------|-------------|------|------|------|
| `.NET: Shared/Contracts` / `.NET: Server/Configuration/SupportServerContracts` | `Shared/src/main/kotlin/.../shared/contracts/Messages.kt` | shared-contract | done | 인증, 상담 생성, 참여, 메시지, typing, close, notification 계약과 Session/Support 사이의 actor 보장 계약을 data class로 추가했다. 2026-07-08 filtered runner가 통과했다. |
| `.NET: Client` | `Client/src/main/kotlin/.../supportchat/client/*` | client | done | agent/customer/reconnect/waiting-customer stream client self-check를 추가했다. `.NET`처럼 `ConversationId`를 stream metadata로 싣는 helper를 두고, assignment/join/message/typing/idle/close/no-agent 검증을 public connector wait path로 수행한다. |
| `.NET: Server/Api` | `Server/Api/src/main/kotlin/.../server/api/*` | server-role | done | `.NET` 기준 token 검증(`customer-1..3`, `agent-1..2`), API channel server, Support channel client, conversation allocation handler, `/health` readiness endpoint를 추가했다. |
| `.NET: Server/Session` | `Server/Session/src/main/kotlin/.../server/session/*` | server-role | done | stream node, session spot mesh, authentication packet, identity actor binding, agent per-conversation actor binding, conversation metadata 기반 relay를 추가했다. |
| `.NET: Server/Support` | `Server/Support/src/main/kotlin/.../server/support/*` | server-role | done | Conversation domain, agent assignment application 계층, wire contract mapper, Support process bootstrap, Support channel server, Support spot mesh, Support user actor/factory/directory, entry spot request handlers, conversation starter, conversation spot, notification publisher를 추가했다. |
| `.NET: Server/Configuration` | `Server/Configuration/src/main/kotlin/.../server/configuration/*` | server-config | done | channel/role 이름, packet 이름, topology, API HTTP readiness endpoint, timing, Redis location store helper를 추가했다. |
| common: 공유 location store | `run_sample.sh`, `run_sample.ps1`, `Server/Configuration/SampleTopology.kt`, `SampleLocationStore.kt` | external-adapter | done | runner가 Redis와 공통 key prefix를 API, Session, Support에 전달한다. 2026-07-08 filtered runner proof를 확보했다. |
| common: client push wait interface | `Client/src/main/kotlin/.../SupportChatClientScenario.kt` | validation | done | `ParticipantJoinedNotify`, `ConversationAssignedNotify`, `ChatMessageNotify`, `TypingChangedNotify`, `ConversationIdleNotify`, `ConversationClosedNotify`를 stream connector public wait path로 기다리는 self-check를 추가했다. |
| common: server evidence | `run_sample.sh`, `run_sample.ps1` | validation | done | runner가 client marker, 닫힌 대화 typing 무시 marker, support 상태 로그, message-flow evidence를 확인한다. 2026-07-08 filtered runner가 통과했다. |
| common: DDD/domain 경계 | `Server/Support/src/main/kotlin/.../domain`, `.../application`, `.../infrastructure/ConversationContracts.kt` | architecture | done | `Conversation`, policy, event, agent availability/assignment service를 framework 타입 없이 추가했고, `ConversationSpot`/handlers/notification publisher가 이 모델을 사용한다. |
| common: bound session push | `Server/Session/src/main/kotlin/.../SupportChatSession.kt`, `Server/Support/src/main/kotlin/.../ConversationNotificationPublisher.kt` | framework-flow | done | Session이 identity actor와 agent conversation actor를 bind하고 bound actor relay를 사용한다. Support conversation spot에서 assignment/message/typing/idle/close notification을 bound session으로 publish한다. |

## 남은 gap

남은 SupportChat Kotlin sample gap은 없다. 기존 재접속 actor-bound session reply blocker는 framework
수정 뒤 2026-07-08 filtered runner에서 닫힌 것으로 확인했다.

## 최근 검증 메모

- `git diff --check -- framework/languages/java/samples/kotlin/SupportChat` 통과.
- `bash -n framework/languages/java/samples/kotlin/SupportChat/run_sample.sh` 통과.
- `pwsh -NoProfile -Command '$null = [scriptblock]::Create((Get-Content -Raw "framework/languages/java/samples/kotlin/SupportChat/run_sample.ps1"))'` 통과.
- `nice -n 10 ../../gradlew --settings-file standalone.settings.gradle.kts --no-daemon --no-parallel --max-workers=1 :Shared:compileKotlin :Server:Configuration:compileKotlin :Server:Api:compileKotlin :Server:Session:compileKotlin :Server:Support:compileKotlin :Client:compileKotlin` 통과. 이 검증은 included build의 `zlink-framework-core:compileJava`와 `zlink-stream-connector:compileJava`를 함께 실행한다.
- `nice -n 10 ../../gradlew --settings-file standalone.settings.gradle.kts --no-daemon --no-parallel --max-workers=1 :zlink-framework-java-build:zlink-framework-core:compileJava :Server:Session:compileKotlin :Server:Support:compileKotlin :Client:compileKotlin` 통과.
- `ZLINK_JAVA_STREAM_TRACE=1 nice -n 10 timeout 300s framework/languages/java/samples/java/SupportChat/run_sample.sh` 통과. 단, 재접속 no-agent 경로 미포함.
- `nice -n 10 timeout 600s env ZLINK_SAMPLE_FILTER=kotlin/SupportChat SUPPORTCHAT_REDIS_ENDPOINT=127.0.0.1:60667 framework/languages/java/samples/run_samples.sh` 통과. 출력은 `All Java/Kotlin samples passed`로 끝났다.
