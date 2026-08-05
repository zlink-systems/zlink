# C++ SupportChat .NET 기준 포팅 inventory

기준 구현: `framework/languages/dotnet/samples/SupportChat`

이 문서는 `.NET SupportChat` 샘플 책임이 C++ 샘플에서 어디에 대응되는지 기록한다.

## 파일 매핑

| .NET 기준 파일/책임 | C++ 대응 파일 | 상태 | 비고 |
|---------------------|---------------|------|------|
| `Shared/Contracts/Messages.cs` | `Shared/Contracts/messages.hpp` | done | 공통 문서의 request, response, notify 이름과 상태 필드를 둔다. |
| `Server/Support/Domain/SupportChat/*` | `Server/Support/Domain/SupportChat/conversation.hpp` | done | conversation 상태, 참여자, 메시지 순서, typing, idle, close 전이를 domain에 둔다. |
| `Server/Support/Application/ConversationAssignment/*` | `Server/Support/Application/ConversationAssignment/agent_assignment_service.hpp` | done | 상담원 availability와 capacity 기반 배정을 둔다. |
| `Server/Configuration/*` | `Server/Configuration/sample_topology.hpp`; `Server/Configuration/location_store.hpp`; `Server/Configuration/role_process.hpp` | done | runner endpoint, Redis location store, Support/Session actor route mesh, flow log 경로를 해석한다. |
| `Server/Api/Program.cs` | `Server/Api/main.cpp` | done | role process entrypoint와 flow evidence target이다. |
| `Server/Session/Program.cs`; `SupportChatSession.cs` | `Server/Session/main.cpp` | done | real packet stream session을 열고, Support channel에서 받은 identity actor ref를 session에 bind한다. agent의 `JoinConversationReq`는 stream metadata의 `ConversationId`로 Support channel에 per-conversation actor 생성을 요청하고, 이후 같은 metadata가 있는 packet은 해당 actor로 relay한다. |
| `Server/Support/Program.cs`; domain/application flow | `Server/Support/main.cpp`; `Server/Support/Domain/SupportChat/conversation.hpp`; `Server/Support/Application/ConversationAssignment/agent_assignment_service.hpp` | done | Support role이 Support channel, Support-owned actor, Entry Spot command handler, `supportchat.conversation` Conversation Spot, public bound-session publisher를 호스팅한다. `EnsureAgentConversationReq`로 agent roster actor와 conversation마다 분리된 actor id를 연결하고 Conversation Spot에 join한다. HTTP self-check도 실제 domain/application 객체로 one-agent-many-conversations, per-room sequence, typing, reconnect state, explicit close, idle close, no-agent waiting을 검증한다. |
| `Client/SupportChatClientScenario.cs` | `Client/supportchat_client_scenario.hpp`; `Client/main.cpp` | done | client가 Support role HTTP self-check evidence를 호출한 뒤 Session stream에 customer/agent connector를 연결한다. conversation packet에는 `ConversationId` metadata를 싣고, public wait interface로 `ConversationAssignedNotify`, `ParticipantJoinedNotify`, `ChatMessageNotify`, `TypingChangedNotify`, `ConversationIdleNotify`, `ConversationClosedNotify`를 기다린다. |
| `run_sample.sh` | `run_sample.sh` | done | 필요한 CMake target을 빌드하고 Redis 준비, role/probe/client 실행, flow trace marker를 검증한다. |
| `SupportChat.csproj`/role csproj | `framework/languages/cpp/CMakeLists.txt` | done | C++ role/client/probe executable과 `sample_smoke` ctest를 등록한다. |

## .NET 파일 대응 보강

| .NET 파일 | C++ 대응 | 상태 | 비고 |
|-----------|----------|------|------|
| `Client/Configuration/SampleNames.cs`; `Client/SupportChat.Client.csproj`; `README.ko.md` | `Shared/Contracts/messages.hpp`; `Client/main.cpp`; `Client/supportchat_client_scenario.hpp`; `README.ko.md` | done | client packet 이름, 실행 진입점, README 실행 설명이 C++ client와 shared contract로 대응된다. |
| `Server/Api/ApiServerHostFactory.cs`; `Server/Api/Handlers/AuthenticateUserHandler.cs`; `Server/Api/Handlers/OpenConversationHandler.cs`; `Server/Api/SupportChat.Server.Api.csproj` | `Server/Api/main.cpp` | done | API role executable이 인증, 대화 열기 HTTP/channel edge와 flow evidence를 맡는다. |
| `Server/Configuration/SampleFlowLog.cs`; `Server/Configuration/SampleNames.cs`; `Server/Configuration/SampleTopology.cs`; `Server/Configuration/SupportChat.Server.Configuration.csproj`; `Server/Configuration/SupportServerContracts.cs` | `Server/Configuration/sample_topology.hpp`; `Server/Configuration/location_store.hpp`; `Server/Configuration/role_process.hpp`; `Shared/Contracts/messages.hpp` | done | endpoint, Redis location store, actor route mesh, role process, support server packet 이름을 C++ configuration/shared header로 모았다. |
| `Server/Session/SessionServerHostFactory.cs`; `Server/Session/SupportChat.Server.Session.csproj` | `Server/Session/main.cpp` | done | Session role executable이 stream endpoint, identity actor binding, conversation metadata relay를 맡는다. |
| `Server/Support/Application/ConversationAssignment/AgentAssignmentService.cs`; `AgentAvailabilityDirectory.cs`; `SupportConversationAllocator.cs` | `Server/Support/Application/ConversationAssignment/agent_assignment_service.hpp` | done | 상담원 availability, capacity, conversation allocation 정책을 application service로 대응한다. |
| `Server/Support/Domain/SupportChat/ConversationModels.cs` | `Server/Support/Domain/SupportChat/conversation.hpp` | done | conversation 상태, 참여자, 메시지 sequence, typing, idle, close domain model을 C++ domain type으로 대응한다. |
| `Server/Support/Infrastructure/ZLink/Actors/SupportActorDirectory.cs`; `SupportUserActor.cs`; `SupportUserActorFactory.cs` | `Server/Support/main.cpp`; `Server/Support/Application/ConversationAssignment/agent_assignment_service.hpp` | done | support user actor directory와 actor 생성은 Support role 안의 actor roster/assignment state로 대응한다. |
| `Server/Support/Infrastructure/ZLink/ConversationStarter.cs`; `Handlers/AllocateConversationHandler.cs`; `EnsureAgentConversationHandler.cs`; `EnsureSupportUserActorHandler.cs` | `Server/Support/main.cpp` | done | conversation 시작, actor 보장, agent conversation 연결 handler를 Support role의 public channel/Spot handler로 대응한다. |
| `Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/ConversationContracts.cs`; `ConversationCreateRequest.cs`; `ConversationSpot.cs`; `Handlers/CloseConversationHandler.cs`; `ConversationIdleTimerHandler.cs`; `JoinConversationHandler.cs`; `SendChatMessageHandler.cs`; `SetTypingHandler.cs`; `Notifications/ConversationNotificationPublisher.cs` | `Server/Support/main.cpp`; `Server/Support/Domain/SupportChat/conversation.hpp`; `Shared/Contracts/messages.hpp` | done | Conversation Spot packet, lifecycle, idle timer, join/message/typing/close handler, notification publisher 책임을 Support role과 domain/shared contract로 대응한다. |
| `Server/Support/Infrastructure/ZLink/Spots/EntrySpot/Handlers/OpenConversationActorHandler.cs`; `SetAgentAvailableHandler.cs`; `SupportEntrySpot.cs`; `Server/Support/SupportServerHostFactory.cs`; `Server/Support/SupportChat.Server.Support.csproj` | `Server/Support/main.cpp` | done | Entry Spot open/availability handler와 Support role host 구성을 C++ Support executable이 맡는다. |
| `Shared/SupportChat.Shared.csproj` | `Shared/Contracts/messages.hpp`; `framework/languages/cpp/CMakeLists.txt` | done | shared project 책임은 header contract와 CMake target include 경로로 대응한다. |

## 남은 gap

현재 sample process에서 확인하지 못한 SupportChat runtime gap은 없다. 공통 계약에 없는
`AuthenticateUser*`, `EnsureSupportUserActor*`, `EnsureAgentConversation*`,
`ConversationCreate*`는 role 사이의 내부 message이고, `supportchat_server_assertion_*`는
test/evidence-only HTTP message다. 이 message들은 public client contract에 추가된 것으로
계산하지 않는다. 6개 sample 전체의 exact inventory와 common E2E 14-config 추적은 별도
ledger gate이므로 이 inventory의 sample process 통과만으로 전체 S1 closure를 판정하지 않는다.

## 검증 기록

- `timeout 300s framework/languages/cpp/samples/SupportChat/run_sample.sh`
  - 결과: 통과
  - 출력: `PASS SupportChat.Cpp`, `supportchat sample result=passed`
  - 의미: client는 HTTP public surface로 Support role domain/application self-check evidence를 확인하고,
    Session role은 stream connector request와 bound-session notification wait까지 실제 TCP stream으로
    검증한다. runner는 `supportchat authentication=verified`,
    `supportchat conversation-assignment=verified`, `supportchat bound-push=verified`,
    `supportchat reconnect=verified`, `supportchat idle-close=verified`, `supportchat=completed`를
    확인한다.
- 2026-08-03: `framework/languages/cpp/samples/run_samples.sh`와
  `pwsh -NoProfile -File framework/languages/cpp/samples/run_samples.ps1`
  - 결과: 두 aggregate 모두 exit code 0
  - 의미: 두 runner가 같은 6개 sample manifest를 선택하고, SupportChat의 public stream,
    role evidence, cleanup 결과를 확인했다. PowerShell 실행은 Linux에서 `bash` runner를
    호출한 결과이므로 native Windows process evidence와는 구분한다.

C++ Support role은 Support-owned actor, Entry Spot, `supportchat.conversation` Conversation Spot을
함께 호스팅한다. Entry Spot은 availability와 open request를 받고, Conversation Spot은 join, message,
typing, close 상태와 participant notification을 소유한다. Session role은 Support channel로 identity
actor와 per-conversation agent actor ref를 받은 뒤 stream packet을 metadata 기준으로 해당 actor에
relay한다. 이전 누락이었던 stream connector 왕복, public wait interface evidence, per-conversation
agent actor, Conversation Spot 분리는 runner가 직접 검증한다.
