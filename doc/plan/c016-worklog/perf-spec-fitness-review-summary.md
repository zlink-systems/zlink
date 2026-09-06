# Framework perf spec 적합성 검토

검토일: 2026-09-06. 독자: 공통 perf spec을 개정하고 .NET 기준 runner의 착수를 결정하는 감독.
읽기 전용 진단이며 이 보고서 외 파일은 수정하지 않았다. 빌드·test·benchmark는 실행하지 않았다.
조회한 HEAD는 `7627284944fe2df46f7f7a3b3a00795b22d5b927`이다. 기존 작업 변경은 보존했다.

**판정: 현재 문구 그대로는 NO-GO, 아래 필수 개정 뒤 .NET 구현 착수는 GO다.** Echo로 측정하려는
주요 사용자 흐름은 public API로 표현할 수 있다. 그러나 자동 turn 반납, ActorRef target,
remote send의 local mailbox 완료라는 가정은 현재 계약과 다르다. Connector만 사용하는 trigger,
public으로 얻을 수 없는 필수 metric, 여러 측정 셀의 파일 충돌도 runner 작성 전에 정해야 한다.
근거는 공통 perf spec `framework/doc/framework/common/perf/README.ko.md:685`, `:762`, `:781`,
`:559`, `:897`, `:946`과 아래 E-TURN·E-ACTOR·E-SUBMIT·E-METRIC 근거 묶음이다.

13개 표준 시나리오는 **11개로 합치는 안**을 권고한다. §10.5·§10.7·§10.8을 하나의
Spot→Channel request 시나리오에서 terminal과 Spot 수가 다른 실행 셀로 유지한다. 기존 질문은
버리지 않는다. §11에는 baseline이 하나가 아니라 **5개** 있으므로 모두 대조했다.
근거: `framework/doc/framework/common/perf/README.ko.md:657`, `:683`, `:699`, `:821`.

이 보고서의 `fits`는 **시나리오의 호출 흐름을 public API로 구성할 수 있다**는 판정이다.
실행 성공·성능·전체 언어 parity를 검증했다는 뜻이 아니다. 공통 CLI·metric 문제는 해당 흐름의
API 유무와 별도로 §4에서 판정한다. 현재 runner 부재는 제공된 inventory의 결론을 사용했다.
근거: `doc/plan/c016-worklog/framework-perf-runner-inventory.md:4`, `:8`.

## 1. Scenario → 현재 public API 매핑

### 1.1 요약표

셀 뒤의 근거 ID는 §1.3의 **실제 public 선언과 exact language interface 문서의 file:line 묶음**이다.
`fits with rename`은 호출 이름·terminal 표기만 대응시키면 되는 경우다. 의미가 달라진 경우는
그 표기로 숨기지 않고 `semantics changed`로 표시했다. C++의 †는 §1.4의 문서·선언 불일치다.

| Scenario / perf spec 근거 | dotnet | cpp | java | kotlin | node |
|---|---|---|---|---|---|
| `cs-local-session-actor-echo` — `framework/doc/framework/common/perf/README.ko.md:595` | fits · D-CS | fits · C-CS† | fits · J-CS | fits · K-CS | fits · N-CS |
| `cs-remote-session-actor-echo` — `framework/doc/framework/common/perf/README.ko.md:613` | fits · D-CS | fits · C-CS† | fits · J-CS | fits · K-CS | fits · N-CS |
| `s2s-channel-to-spot-request-echo` — `framework/doc/framework/common/perf/README.ko.md:630` | fits · D-ROUTE | fits · C-ROUTE | fits · J-ROUTE | fits · K-ROUTE | fits · N-ROUTE |
| `s2s-channel-to-spot-send-send-echo` — `framework/doc/framework/common/perf/README.ko.md:643` | fits · D-ROUTE | fits · C-ROUTE | fits · J-ROUTE | fits · K-ROUTE | fits · N-ROUTE |
| `s2s-spot-to-channel-request-echo` — `framework/doc/framework/common/perf/README.ko.md:657` | fits · D-ROUTE | fits · C-ROUTE | fits · J-ROUTE | fits · K-ROUTE | fits · N-ROUTE |
| `s2s-spot-to-channel-send-send-echo` — `framework/doc/framework/common/perf/README.ko.md:670` | fits · D-ROUTE | fits · C-ROUTE | fits · J-ROUTE | fits · K-ROUTE | fits · N-ROUTE |
| `spot-async-request-echo` — `framework/doc/framework/common/perf/README.ko.md:683` | semantics changed · D-TURN | semantics changed · C-TURN | semantics changed · J-TURN | semantics changed · K-TURN | semantics changed · N-TURN |
| `spot-await-contention` — `framework/doc/framework/common/perf/README.ko.md:699` | semantics changed · D-TURN | semantics changed · C-TURN | semantics changed · J-TURN | semantics changed · K-TURN | semantics changed · N-TURN |
| `spot-no-await-echo` — `framework/doc/framework/common/perf/README.ko.md:719` | fits · D-ROUTE | fits · C-ROUTE | fits · J-ROUTE | fits · K-ROUTE | fits · N-ROUTE |
| `spot-worker-offload-echo` — `framework/doc/framework/common/perf/README.ko.md:736` | semantics changed · D-TURN | semantics changed · C-TURN | semantics changed · J-TURN | semantics changed · K-TURN | semantics changed · N-TURN |
| `actor-no-bind-request-echo` — `framework/doc/framework/common/perf/README.ko.md:760` | semantics changed · D-ACTOR | semantics changed · C-ACTOR | semantics changed · J-ACTOR | semantics changed · K-ACTOR | semantics changed · N-ACTOR |
| `actor-no-bind-send-send-echo` — `framework/doc/framework/common/perf/README.ko.md:779` | semantics changed · D-ACTOR | semantics changed · C-ACTOR | semantics changed · J-ACTOR | semantics changed · K-ACTOR | semantics changed · N-ACTOR |
| `pubsub-fanout-echo` — `framework/doc/framework/common/perf/README.ko.md:794` | fits · D-FANOUT | fits with rename · C-FANOUT | fits with rename · J-FANOUT | fits with rename · K-FANOUT | fits with rename · N-FANOUT |
| baseline `connector-echo-only` — `framework/doc/framework/common/perf/README.ko.md:823` | no public API · D-CS / E-SESSION | no public API · C-CS / E-SESSION | no public API · J-CS / E-SESSION | no public API · K-CS / E-SESSION | no public API · N-CS / E-SESSION |
| baseline `session-echo-only` — `framework/doc/framework/common/perf/README.ko.md:824` | fits · D-CS | fits · C-CS† | fits · J-CS | fits · K-CS | fits · N-CS |
| baseline `channel-echo-only` — `framework/doc/framework/common/perf/README.ko.md:825` | fits · D-ROUTE | fits · C-ROUTE | fits · J-ROUTE | fits · K-ROUTE | fits · N-ROUTE |
| baseline `spot-local-echo` — `framework/doc/framework/common/perf/README.ko.md:826` | fits · D-ROUTE | fits · C-ROUTE | fits · J-ROUTE | fits · K-ROUTE | fits · N-ROUTE |
| baseline `spot-no-await-echo` — `framework/doc/framework/common/perf/README.ko.md:827` | fits · D-ROUTE | fits · C-ROUTE | fits · J-ROUTE | fits · K-ROUTE | fits · N-ROUTE |

`connector-echo-only`의 판정은 “session 없이 connector dispatch와 codec만”이라는 현재 정의에 대한
것이다. Framework STREAM server는 session callback을 거치므로 session을 빼려면 별도 wire echo
server나 Core/binding 수신 loop가 필요하다. 그것은 이 perf의 public-only 범위를 벗어난다.
`session-echo-only`로 같은 경로를 측정하면서 이름만 connector-only라고 부르는 것도 맞지 않는다.
근거: E-SESSION, `framework/doc/framework/common/perf/README.ko.md:823`, `:1294`.

### 1.2 시나리오별 client·server·완료 흐름

아래 `request`·`send`는 §1.3에서 해당 언어의 실제 method와 terminal로 치환한다. `ActorCaller`,
Channel 및 Spot 부하의 외부 trigger는 측정용 application endpoint를 호출하는 별도 client다.
현행 connector-only trigger의 충돌은 §4.1에서 개정 대상으로 남겼다.
근거: `framework/doc/framework/common/perf/README.ko.md:185`, `:559`, `:762`.

| Scenario | Public 호출로 표현한 전체 흐름과 측정 경계 | 소유 계약 / 현재 근거 |
|---|---|---|
| `cs-local-session-actor-echo` | Client connector typed `Request` → session callback → 준비 단계에서 `ActorRef`를 bind한 session actor의 relay → typed Actor request handler의 reply → 원래 STREAM request 완료. 같은 process뿐 아니라 **같은 local object node인지**도 고정해야 local 경로가 된다. | E-SESSION. .NET sample `framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Sessions/PlaySession.cs:53`; 배치 근거 `framework/doc/framework/common/sample/tictactoe/README.ko.md:14`. Perf `:597`의 local 전제는 유지 가능하다. |
| `cs-remote-session-actor-echo` | Client connector request → Object Client인 SessionServer의 bound actor relay → 별도 Object Server의 Actor handler → original STREAM correlation reply. Bind·Actor 생성은 connect/warmup 이전에 끝낸다. | E-SESSION; `framework/doc/framework/common/e2e/config-2-spot-service.ko.md:28`; sample `framework/languages/dotnet/samples/Bingo/Server/Session/Sessions/BingoSession.cs:36`, 아래 교차언어 sample 대조. |
| `s2s-channel-to-spot-request-echo` | 외부 client는 trigger만 보낸다. ChannelServer가 **Object Client도 제공하는 MeshNode host**에서 global SpotId request를 시작하고 remote User Spot typed handler의 reply로 server-local RTT를 완결한다. | E-SPOT; Object Client와 Channel Server의 공존 `framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:126`; E2E `framework/doc/framework/common/e2e/config-2-spot-service.ko.md:70`. |
| `s2s-channel-to-spot-send-send-echo` | ChannelServer가 correlation을 등록하고 global SpotId로 send → Spot handler가 outbound Channel send로 전용 echo-return Channel Server에 reply DTO를 보낸다 → 그 send handler가 correlation 완료. 두 send의 public 수락 완료와 왕복 완료를 구분한다. | E-SUBMIT, E-SPOT; `framework/doc/framework/common/perf/README.ko.md:645`, `:859`. Return Channel은 caller process 하나만 Server여야 select-one으로 다른 caller에 응답이 가지 않는다(E-CHANNEL). |
| `s2s-spot-to-channel-request-echo` | Trigger를 받은 User Spot handler가 outbound Channel request → 별도 ChannelServer typed request handler → Spot handler에서 reply 완료. 일반 terminal을 쓰면 해당 Spot turn을 유지한다. | E-TURN, E-CHANNEL; 실제 사용 예 `framework/languages/dotnet/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/BingoRoom.cs:93`. Channel이 RouteMesh인지 ClientServer인지 perf 본문은 정하지 않았으므로 §4에서 고정해야 한다. |
| `s2s-spot-to-channel-send-send-echo` | Spot handler가 correlation을 등록하고 Channel send → Channel handler가 **설정된 source SpotId**로 send → 원래 Spot의 send handler가 correlation 완료. Channel message context에 source SpotId가 반드시 들어 있다고 가정하지 않는다. | E-SPOT, E-CHANNEL; `.NET` context가 제공하는 값은 `framework/languages/dotnet/src/Zlink.Framework/Contracts/Channels/RouteCalls.cs:60`; perf DTO에는 return SpotId가 없다(`framework/doc/framework/common/perf/README.ko.md:838`). 다수 Spot이면 payload/application metadata에 return SpotId를 명시해야 한다. |
| `spot-async-request-echo` | Public remote request 자체는 가능하다. 다만 현재 일반 `Async/async/submit/await`는 turn을 유지한다. 문서가 의도한 반납·재개 비용을 재려면 `SpotWide` User Spot에서 **`Yield/yield`**를 선택해야 한다. | E-TURN; 공통 E2E `framework/doc/framework/common/e2e/config-8-execution-turn.ko.md:53`, `:122`. |
| `spot-await-contention` | 바로 위와 동일한 public 경로에 SpotId 수만 1이다. 여러 logical caller가 같은 Spot으로 보낸다. 같은 Actor 하나의 FIFO에 몰아넣으면 `Yield` 뒤에도 Actor claim이 유지되므로 다른 실험이다. | E-TURN; `framework/doc/framework/common/e2e/config-8-execution-turn.ko.md:264`, `:281`; perf `framework/doc/framework/common/perf/README.ko.md:715`. |
| `spot-no-await-echo` | Application trigger handler → 같은 SpotServer의 public Spot request → User Spot typed handler가 payload를 즉시 반환한다. RemoteEcho는 없다. Handler 자체의 동작은 public으로 표현된다. | E-SPOT, E-TURN; `framework/doc/framework/common/perf/README.ko.md:721`. 결과에는 trigger·codec·local dispatch 포함 범위를 써야 하며 “Spot dispatch만”이라는 순수 비용으로 해석하지 않는다. |
| `spot-worker-offload-echo` | Spot request handler → `RunCpuWorker/run_cpu_worker/runCpuWorker` → bounded worker에서 고정 CPU 작업 → `Yield/yield`로 원래 Spot gate에서 재개 → echo. 일반 terminal variant는 turn 유지 대조군이다. | E-TURN. Node callback은 self-contained이며 structured-clone 가능한 결과만 반환한다(`framework/languages/node/packages/framework/src/contracts/Spots/Contracts.ts:50`). 원문 “busy-wait 또는 sleep”은 동등 CPU 부하가 아니다(`framework/doc/framework/common/perf/README.ko.md:743`). |
| `actor-no-bind-request-echo` | Session이 없는 Object Client `ActorCaller`가 **ActorId** request → remote Actor typed request handler → caller reply. ActorRef로 incarnation을 고정하는 메시징은 제공하지 않는다. | E-ACTOR; `framework/doc/framework/common/e2e/config-9-to-actor-messaging.ko.md:7`, `:30`. |
| `actor-no-bind-send-send-echo` | ActorCaller가 ActorId send → Actor send handler가 주입된 public Channel client로 전용 caller Channel에 send → caller handler correlation 완료. `BoundSession.Send`를 쓰면 no-bind 전제를 깨므로 사용할 수 없다. | E-ACTOR, E-SUBMIT; `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:598`. Remote target의 최초 send 완료는 source transport queue 수락이다. |
| `pubsub-fanout-echo` | Publisher의 **Classic fanout client** `Publish/publish` → 별도 Subscriber process의 typed fanout handler → 각 subscriber가 unique sequence·latency를 기록. Publish terminal은 로컬 수락, delivery는 수신 측 evidence다. | E-FANOUT. `Spot.Outbound.Publish`는 Logical Multicast이므로 대체할 수 없다(E-SPOT). `framework/doc/framework/common/e2e/config-3-pubsub.ko.md:44`. |
| baseline `session-echo-only` | Client connector request → session typed handler → session client `Reply/reply` 또는 C++ `reply_packet` → client 완료. Actor를 만들지 않는다. | E-SESSION, D/J/K/N-CS, C-CS†. |
| baseline `channel-echo-only` | Application trigger → source의 Channel client request/send → remote Channel handler reply/send. RouteMesh와 ClientServer 중 무엇인지 결과와 설정에 고정한다. | E-CHANNEL. `framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md:151`, `:160`은 두 경로의 local 처리도 다르게 정한다. |
| baseline `spot-local-echo` / `spot-no-await-echo` | 동일 process의 public Spot client → local Ready User Spot typed echo. 같은 trigger와 완료 경계를 고정하면 두 baseline과 §10.9는 동일 실행 셀이다. | E-SPOT; `framework/doc/framework/common/perf/README.ko.md:721`, `:826`. Local dispatch를 위해 handler를 직접 호출하지 않는다. |

### 1.3 Public API 근거 묶음

아래는 sample helper가 아니라 실제 공개 선언을 먼저 대조한 목록이다. 같은 묶음을 참조한 여러
시나리오는 동일 API의 request/send 또는 local/remote 배치 차이다.

**공통 의미를 소유하는 절**

| ID | 현재 규칙과 file:line |
|---|---|
| E-SESSION | Session callback 필수·raw recv loop 금지: `framework/doc/framework/common/spec/server/04-session/01-stream-session.ko.md:18`, `:31`, `:94`. Bind·relay·original reply: `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:144`, `:628`, `:724`. |
| E-CHANNEL | ChannelName select-one과 ClientServer/RouteMesh의 차이: `framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md:151`, `:160`, `:170`; ClientServer request/reply: `framework/doc/framework/common/spec/server/02-channel-transport/03-client-server-channel.ko.md:338`. |
| E-SPOT | Global SpotId, NodeRid와의 구분, Store resolve: `framework/doc/framework/common/spec/server/03-spot-actor/06-spot-address-messaging.ko.md:33`, `:107`; Spot outbound Channel 호출: `framework/doc/framework/common/spec/server/03-spot-actor/02-spot-messaging.ko.md:413`; Logical Multicast/Classic fanout 구분: 같은 파일 `:240`, `:260`, `:517`. |
| E-ACTOR | Direct message는 global ActorId, ActorRef·MeshName·owner RID target 금지: `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:273`, `:284`; cache·non-replay·binding 독립: 같은 파일 `:291`. |
| E-TURN | 일반 terminal은 turn 유지, Yield만 shared gate 반납: `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:44`, `:50`, `:55`; worker 및 오류: 같은 파일 `:76`; E2E `framework/doc/framework/common/e2e/config-8-execution-turn.ko.md:8`, `:230`, `:281`. |
| E-SUBMIT | One-way source-local admission의 remote/local/socket 구분: `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:100`; deadline·target 선택: 같은 파일 `:136`, `:170`, `:200`; request 첫 terminal·non-replay: 같은 파일 `:263`, `:288`. |
| E-QUEUE | 두 capacity authority·permit 반환·continuation·topology별 completion·pressure: `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:16`, `:75`, `:116`, `:126`, `:253`, `:333`. |
| E-METRIC | Host aggregate만 public이며 owner별 목록 없음: `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:133`, `:168`; per-job histogram·per-turn 계측 없음: `framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.ko.md:82`, `:319`. |
| E-ERROR | 13개 public ErrorKind와 내부 원인 비노출: `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md:21`, `:42`; send/request 오류: 같은 파일 `:59`, `:75`. |
| E-FANOUT | Classic fanout non-replay·local admission 및 subscriber evidence: `framework/doc/framework/common/e2e/config-3-pubsub.ko.md:5`, `:44`; 공통 완료 의미 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:196`. |

**.NET**

| ID | 현재 호출 / 실제 public 선언 | Exact interface 문서 |
|---|---|---|
| D-CS | Connector `Request(dto).Async<TReply>()`: `framework/languages/dotnet/src/Systems.Zlink.Stream.Connector/Contracts/ZlinkStreamTypedConnectorExtensions.cs:68`, `:368`. Session `OnDispatchAsync`, `Actors.BindAsync/BindOrGetAsync`, `Client.Reply(dto)`: `framework/languages/dotnet/src/Zlink.Framework/Contracts/Streams/IZLinkSession.cs:44`, `:60`, `:67`. Bound actor `RelayAsync(payload)`: `framework/languages/dotnet/src/Zlink.Framework/Contracts/Streams/IZLinkSessionActor.cs:9`. | `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md:58`, `:67`, `:89`, `:101`, `:172`. |
| D-ROUTE | Root `IZLinkSpotClient.RequestToSpot/SendToSpot`: `framework/languages/dotnet/src/Zlink.Framework/Contracts/Spots/ZLinkSpot.cs:184`. Spot `Outbound.RequestToChannel/SendToChannel`: 같은 파일 `:197`, `:201`. Route client `RequestToChannel/SendToChannel`와 typed route handlers: `framework/languages/dotnet/src/Zlink.Framework/Contracts/Channels/RouteCalls.cs:15`, `:24`, `:32`. | `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/05-spots.ko.md:147`, `:520`; `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/04-channel-messaging.ko.md:44`, `:68`, `:86`. |
| D-TURN | Channel request `.Async<TReply>()/.Yield<TReply>()`: `framework/languages/dotnet/src/Zlink.Framework/Contracts/Channels/Calls.cs:23`. `RunCpuWorker`(`framework/languages/dotnet/src/Zlink.Framework/Contracts/Spots/ZLinkSpot.cs:259`)와 worker `.Async()/.Yield()`: interface 문서 `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/05-spots.ko.md:174` 및 `framework/languages/dotnet/src/Zlink.Framework/Contracts/Workers/ZLinkWorkers.cs:3`; worker `MaxThreads`: 같은 파일 `:18`. | `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/01-common-runtime.ko.md:46`, `:67`, `:111`; `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/05-spots.ko.md:174`. |
| D-ACTOR | `SendToActor(actorId,dto).Async()`, `RequestToActor(actorId,dto).Async<TReply>()`: `framework/languages/dotnet/src/Zlink.Framework/Contracts/Actors/IZLinkActorClient.cs:3`. 준비 단계 `Create/GetOrCreate`: `framework/languages/dotnet/src/Zlink.Framework/Contracts/Actors/IZLinkActorManager.cs:5`; Actor factory 등록 `framework/languages/dotnet/src/Zlink.Framework/Contracts/Configuration/MeshNodeBuilders.cs:87`. | `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/06-actors.ko.md:106`, `:109`, `:184`. |
| D-FANOUT | `IZLinkFanoutClient.Publish(channel,topic,event)`와 `.Async()`: `framework/languages/dotnet/src/Zlink.Framework/Contracts/Channels/IZLinkFanoutClient.cs:1`; `framework/languages/dotnet/src/Zlink.Framework/Contracts/Channels/Calls.cs:43`. Typed subscriber `IZLinkFanoutHandler`. | `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/04-channel-messaging.ko.md:131`, `:143`; topology `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/03-configuration-topology.ko.md:249`. |
| D-STATUS | `IZLinkFrameworkRuntime.Status`, `ResetCapacityMetrics()`와 capacity DTO: `framework/languages/dotnet/src/Zlink.Framework/Contracts/Configuration/ZLinkDrainContracts.cs:137`, `:172`. | `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md:102`, `:132`, `:170`. |

**C++**

| ID | 현재 호출 / 실제 public 선언 | Exact interface 문서 |
|---|---|---|
| C-CS | Connector `request(dto)`·`dispatch()`: `framework/languages/cpp/connector/core/include/zlink/stream_connector/contracts/zlink_stream_connector.hpp:110`, `:220`. Session `stream.actors()`, `reply_packet`와 `packet_stream_session_t`: `framework/languages/cpp/framework/include/zlink/framework/contracts/streams/stream.hpp:213`, `:216`, `:252`. 실제 Actor relay는 `relay_request(payload).async()` / `relay(payload)`: `framework/languages/cpp/framework/include/zlink/framework/contracts/actors/actor.hpp:874`. | `framework/doc/framework/common/spec/server/languages/cpp/interfaces/06-stream-session.ko.md:78`, `:180`, `:206`; `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md:281`. †차이는 §1.4. |
| C-ROUTE | `route_client_t.send_to_spot/request_to_spot`, `send_to_channel/request_to_channel`: `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp:798`, `:819`, `:864`, `:885`. Spot context direct calls: `framework/languages/cpp/framework/include/zlink/framework/contracts/spots/spot.hpp:960`. | `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:965`, `:968`; outbound·handler `framework/doc/framework/common/spec/server/languages/cpp/interfaces/04-spots.ko.md:206`, `:209`, `:487`. |
| C-TURN | `run_cpu_worker`: `framework/languages/cpp/framework/include/zlink/framework/contracts/spots/spot.hpp:980`; worker `.async()/.yield()`: `framework/languages/cpp/framework/include/zlink/framework/contracts/workers/worker.hpp:418`; `max_threads`: 같은 파일 `:46`. Channel request `.async<TReply>()/.yield<TReply>()`(`framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp:157`). | `framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.ko.md:333`, `:350`, `:362`; request `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:693`, `:705`. |
| C-ACTOR | `actor_client_t.send(actor_id_t,dto).async()`, `.request(actor_id_t,dto).async<TReply>()`: `framework/languages/cpp/framework/include/zlink/framework/contracts/actors/actor.hpp:317`, `:357`. | `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md:152`, `:178`, `:192`. |
| C-FANOUT | `publisher_t` 계열의 `publish(channel,topic,event)` → `fanout_publish_call_t.async()`: `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp:1108`. | `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:881`, `:1027`. |
| C-STATUS | `framework_runtime_t` public status/reset: `framework/languages/cpp/framework/include/zlink/framework/contracts/monitoring/framework_runtime.hpp:153`. | `framework/doc/framework/common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md:106`. |

**Java**

| ID | 현재 호출 / 실제 public 선언 | Exact interface 문서 |
|---|---|---|
| J-CS | Connector `request(Object)` → typed request call: `framework/languages/java/zlink-stream-connector/src/main/java/systems/zlink/stream/connector/ZLinkStreamConnector.java:61`. Session `bind(ActorRef)/bindOrGet`, `relay(dispatch,payload)`, `client().reply(dto).submit()`: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/streams/ZLinkSessionActors.java:14`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/streams/ZLinkSessionActor.java:15`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/streams/ZLinkSessionClient.java:6`. | `framework/doc/framework/common/spec/server/languages/java/interfaces/stream-session.ko.md:55`, `:63`, `:70`, `:76`, `:137`. |
| J-ROUTE | `ZLinkRouteClient.requestToSpot/sendToSpot`, `requestToChannel/sendToChannel`: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/channels/ZLinkRouteClient.java:8`, `:21`, `:30`. Spot `outbound()` methods: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/spots/ZLinkSpotOutbound.java:8`, `:21`. | `framework/doc/framework/common/spec/server/languages/java/interfaces/channel-messaging.ko.md:90`, `:235`, `:244`; `framework/doc/framework/common/spec/server/languages/java/interfaces/spots.ko.md:606`. |
| J-TURN | 일반 `.submit(Reply.class)` / `.yield(Reply.class)`; `runCpuWorker`: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/spots/ZLinkSpotContext.java:24`. Worker `.submit()/.yield()`: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/spots/ZLinkWorkerCall.java:19`. Worker size: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/configuration/ZLinkWorkerOptions.java:16`. | `framework/doc/framework/common/spec/server/languages/java/interfaces/channel-messaging.ko.md:235`, `:315`; `framework/doc/framework/common/spec/server/languages/java/interfaces/spots.ko.md:102`, `:671`. |
| J-ACTOR | `sendToActor(String,Object)`, `requestToActor(String,Object)`와 `.submit/.yield`: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/actors/ZLinkActorClient.java:4`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/actors/ZLinkActorRequestCall.java:11`. | `framework/doc/framework/common/spec/server/languages/java/interfaces/actors.ko.md:170`, `:252`. |
| J-FANOUT | `ZLinkFanoutClient.publish(channel,topic,event).submit()`: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/channels/ZLinkFanoutClient.java:4`. | `framework/doc/framework/common/spec/server/languages/java/interfaces/channel-messaging.ko.md:95`, `:208`. |
| J-STATUS | `ZLinkFrameworkRuntime.status()/resetCapacityMetrics()`: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:941`, `:945`; capacity record `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/monitoring/ZLinkHostCapacityStatus.java:6`. | `framework/doc/framework/common/spec/server/languages/java/interfaces/monitoring.ko.md:34`, `:64`, `:106`. |

**Kotlin**

Kotlin 구현은 별도 `framework/languages/kotlin/`가 아니라 Java build root의 Kotlin wrapper와
sample에 있다. Perf §17.3의 위치는 현재 구조와 맞는다.
근거: `framework/doc/framework/common/perf/README.ko.md:1170`; 아래 K-CS·K-ROUTE 실제 경로.

| ID | 현재 호출 / 실제 public 선언 | Exact interface 문서 |
|---|---|---|
| K-CS | Kotlin connector `request(dto).awaitReply<T>()`: `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkConnectorExtensions.kt:150`, `:192`. `bindOrGetActor(ref)`: `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:98`. Session wrapper `relay(dispatch,payload).await()`, `reply(dto).await()`: `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:451`, `:503`. | `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/stream-session.ko.md:77`, `:95`, `:99`. |
| K-ROUTE | `ZLinkKotlinRouteClient.requestToSpot<T>()/sendToSpot`, `requestToChannel<T>()/sendToChannel` → `.await()`: `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:282`, `:288`, `:545`, `:557`, `:567`. Java Spot outbound도 그대로 접근 가능하며 Kotlin bridge로 기다린다. | `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/channel-messaging.ko.md:44`, `:61`, `:68`; `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/spots.ko.md:259`. |
| K-TURN | Request wrapper `.await()/.yield()` 및 Java call의 `awaitReply/yieldReply`: `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:124`, `:129`; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:33`, `:39`. Java `runCpuWorker(...).kotlin().await()/yield()`: `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:521`. Worker options는 J-TURN을 사용한다. | `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/channel-messaging.ko.md:152`; `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/spots.ko.md:395`, `:423`. |
| K-ACTOR | `sendToActor(actorId,dto).await()`, `requestToActor<T>(actorId,dto).await()`: `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:299`, `:305`, `:551`. | `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/actors.ko.md:157`, `:162`, `:169`. |
| K-FANOUT | `ZLinkKotlinFanoutClient.publish(...).await()`: `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:230`, `:237`. | `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/channel-messaging.ko.md:87`. |
| K-STATUS | Java runtime status/reset을 그대로 사용한다(J-STATUS). | `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/monitoring.ko.md:12`. |

**Node.js**

| ID | 현재 호출 / 실제 public 선언 | Exact interface 문서 |
|---|---|---|
| N-CS | Connector `request(dto)`와 `dispatch()`: `framework/languages/node/packages/stream-connector/src/Contracts/IZlinkStreamConnector.ts:43`. Session `actors.bindOrGet(ref)`, `relay(dispatch,payload)`: `framework/languages/node/packages/framework/src/contracts/Streams/IZLinkSessionActor.ts:6`, `:14`. `client.reply(dto).submit()`: `framework/languages/node/packages/framework/src/contracts/Streams/IZLinkSession.ts:39`, `:51`. | `framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md:356`, `:365`, `:374`, `:381`, `:460`. |
| N-ROUTE | `ZLinkRouteClient.requestToChannel/sendToChannel`: `framework/languages/node/packages/framework/src/contracts/Channels/RouteCalls.ts:5`. Spot outbound `sendToSpot/requestToSpot`, `sendToChannel/requestToChannel`: `framework/languages/node/packages/framework/src/contracts/Spots/Contracts.ts:85`. Channel terminal `.submit<T>()`: `framework/languages/node/packages/framework/src/contracts/Channels/Calls.ts:13`. | `framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md:240`, `:252`; `framework/doc/framework/common/spec/server/languages/node/interfaces/04-spots.ko.md:177`. |
| N-TURN | Channel `.yield<T>()`: `framework/languages/node/packages/framework/src/contracts/Channels/Calls.ts:17`. CPU worker callback·`.submit/.yield`: `framework/languages/node/packages/framework/src/contracts/Spots/Contracts.ts:31`, `:50`. | `framework/doc/framework/common/spec/server/languages/node/interfaces/06-stream-worker.ko.md:148`, `:154`, `:168`; `framework/doc/framework/common/spec/server/languages/node/interfaces/04-spots.ko.md:117`, `:391`. |
| N-ACTOR | `sendToActor(actorId,dto).submit()`, `requestToActor(actorId,dto).submit<T>()`: `framework/languages/node/packages/framework/src/contracts/Actors/ZLinkActorClient.ts:3`. | `framework/doc/framework/common/spec/server/languages/node/interfaces/05-actors.ko.md:84`, `:85`, `:125`. |
| N-FANOUT | `IZLinkFanoutClient.publish(channel,topic,event).submit()`: `framework/languages/node/packages/framework/src/contracts/Channels/IZLinkFanoutClient.ts:10`; `framework/languages/node/packages/framework/src/contracts/Channels/Calls.ts:32`. | `framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md:92`, `:127`. |
| N-STATUS | `runtime.status.capacity`, `runtime.resetCapacityMetrics()`: `framework/languages/node/packages/framework/src/contracts/RouteMesh/RuntimeTopology.ts:73`, `:140`, `:146`. | `framework/doc/framework/common/spec/server/languages/node/interfaces/03-location-observability.ko.md:235`, `:257`, `:312`. |


Node의 channel handler → Spot 호출은 주입받은 `ZLinkSpotOutbound`로 실현한다.
`ZLINK_SPOT_OUTBOUND`는 공개 token이고 package root에서 export되며, Bingo의 channel request handler가
그 token으로 주입받아 `requestToSpot(...).instanceSpot(...).inMesh(...).submit()`을 호출한다
(`framework/languages/node/packages/nestjs/src/tokens.ts:16`,
`framework/languages/node/packages/nestjs/src/index.ts:39`,
`framework/languages/node/samples/Bingo.Ts/Server/Api/Handlers/match-bingo-handler.ts:20`).
따라서 위 매핑의 `fits`는 이 public DI 경로를 근거로 한다. 다만 Node interface 문서는
`ZLinkRouteClient`에 `sendToSpot/requestToSpot`을 싣고 실제 그 interface는 channel/node 호출만
선언한다. 이 선언 차이를 runner에서 internal cast로 보충하면 안 된다
(`framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md:247`,
`framework/languages/node/packages/framework/src/contracts/Channels/RouteCalls.ts:4`,
`framework/languages/node/packages/framework/src/contracts/Spots/Contracts.ts:84`).

### 1.4 Sample·E2E 교차언어 대조와 제한

| 흐름 | 실제 사용 근거 |
|---|---|
| Local session→Actor | .NET `framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Sessions/PlaySession.cs:67`; C++ `framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Sessions/play_session.hpp:66`; Kotlin `framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/sessions/PlaySession.kt:27`; Node `framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Sessions/play-session.ts:16`. 공통 배치 `framework/doc/framework/common/sample/tictactoe/README.ko.md:14`. |
| Remote session→Actor | C++ `framework/languages/cpp/samples/Bingo/Server/Session/Sessions/bingo_session.hpp:69`; Java `framework/languages/java/samples/java/Bingo/Server/Session/src/main/java/systems/zlink/samples/bingo/server/session/sessions/BingoSession.java:65`; Kotlin `framework/languages/java/samples/kotlin/Bingo/Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/bingo/server/session/sessions/BingoSession.kt:44`; Node `framework/languages/node/samples/Bingo.Ts/Server/Session/Sessions/bingo-session.ts:22`. 역할 계약 `framework/doc/framework/common/e2e/config-2-spot-service.ko.md:28`. |
| Spot→Channel 대기 | .NET `framework/languages/dotnet/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/BingoRoom.cs:91`; C++ `framework/languages/cpp/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp:185`; Java `framework/languages/java/samples/java/Bingo/Server/Play/src/main/java/systems/zlink/samples/bingo/server/play/infrastructure/zlink/spots/bingoroomspot/BingoRoomSpot.java:103`; Kotlin `framework/languages/java/samples/kotlin/Bingo/Server/Play/src/main/kotlin/systems/zlink/samples/kotlin/bingo/server/play/infrastructure/zlink/spots/bingoroomspot/BingoRoomSpot.kt:97`. 네 구현 모두 위 지점에서 Yield 계열을 사용한다. |
| Node의 대조 차이 | 같은 Bingo 위치는 `.submit()`이다: `framework/languages/node/samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts:172`, `:196`. 따라서 sample이 5언어 모두 Yield 사용의 증거라고 쓰지 않는다. Public Yield의 존재는 N-TURN, 의미는 E-TURN이 증명한다. 공통 sample 안내 `framework/doc/framework/common/sample/README.ko.md:297`와 Node sample 사용 방식의 차이는 별도 sample parity 검토 사항이다. |
| No-bind actor send | .NET `framework/languages/dotnet/samples/DeliveryDispatch/Server/Tracking/Handlers.cs:28`; 공통 E2E의 명시적 sessionless caller `framework/doc/framework/common/e2e/config-9-to-actor-messaging.ko.md:30`, `:44`. |
| Classic fanout | Java `framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/ops/OpsSession.java:85`; Kotlin `framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/ops/OpsSession.kt:49`; Node `framework/languages/node/samples/ZoneWorld/Server/Ops/ops-handlers.ts:110`; E2E `framework/doc/framework/common/e2e/config-3-pubsub.ko.md:24`. |

**C++ †:** exact interface는 `relay(const message_t&)`와 dispatch-context overload를 선언하지만
현재 public header는 `zlink::message_t`를 받는 `relay`와 별도 `relay_request`를 선언하고 sample도
후자를 사용한다. Header의 public method이므로 “internals 없이는 구현 불가”로 분류하지는 않았다.
다만 공통 Framework message/relay 계약에 일치한 source인지 C++ 구현 착수 전에 소유자가 확정해야 한다.
Perf가 자체 raw frame codec을 만드는 해법은 금지다.
근거: `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md:286`,
`framework/doc/framework/common/spec/server/languages/cpp/interfaces/06-stream-session.ko.md:198`,
`framework/languages/cpp/framework/include/zlink/framework/contracts/actors/actor.hpp:874`,
`framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Sessions/play_session.hpp:68`.

## 2. 0.17.0 기준으로 무효화됐거나 명시가 필요한 가정

0.17.0은 현재 사용 package 기준이며 모든 문구 변화가 그 버전 하나에서 생겼다고 단정하지 않는다.
.NET·JVM·Node의 package 입력은 각각 `framework/languages/dotnet/Directory.Packages.props:5`,
`framework/languages/java/gradle/libs.versions.toml:2`, `framework/languages/node/package.json:59`다.

| Perf의 가정 / 누락 | 현재 계약과 최소 정정 |
|---|---|
| §1·§10의 “일반 Async가 자동 반납” — `framework/doc/framework/common/perf/README.ko.md:24`, `:685`, `:701`, `:738` | **무효.** E-TURN의 일반 terminal은 turn 유지, Yield만 반납이다. SpotWide User Spot/Instance Spot이어야 한다. Entry Spot·PerActor·owner turn 밖 Yield는 operation 제출 전에 실패한다. 명시적 terminal과 실행 모드를 기록한다. |
| §10.11/12 ActorRef messaging·local mailbox 완료 — `framework/doc/framework/common/perf/README.ko.md:762`, `:782`, `:895` | **무효.** E-ACTOR는 ActorId로 current Ready incarnation을 찾는다. Remote send는 source transport queue 수락이다(E-SUBMIT). ActorRef는 준비 단계 create 결과와 session bind에만 사용한다. `actor.localHandoff`는 `actor.sourceAdmission`으로 고친다. |
| §10·§14의 전용 오류 이름 — `framework/doc/framework/common/perf/README.ko.md:754`, `:774`, `:894` | **무효.** `WorkerQueueFull`, `WorkerTimeout`, `ActorRouteNotFound`, `ActorLocationStale`, `RouteNotConnected`를 공통 public error kind로 요구할 수 없다. E-ERROR의 `CapacityExceeded`, `DeadlineExceeded`, `NotFound`, `Unavailable` 등 실제 kind를 기록한다. 내부 reason을 exception 문자열 parsing으로 복원하지 않는다. |
| §13이 request와 send/send의 완료 대기만 비교 — `framework/doc/framework/common/perf/README.ko.md:858`, `:866` | **불충분.** Public call 시작부터 source admission 대기를 포함한 logical operation을 한 번 센다. Send terminal을 reply로 세지 않는다. Application shared queue wait, owner FIFO 포화 오류, worker queue 오류를 같은 “HWM error”로 합치지 않는다(E-SUBMIT·E-QUEUE·E-ERROR). |
| DONTWAIT wait-token 제출 모델이 계측 경계에 없음 — `framework/doc/framework/common/perf/README.ko.md:326`, `:878` | Core의 즉시 SEND 성공 ID는 0이고, DONTWAIT backpressure는 payload 없는 nonzero wait token이다. WRITABLE은 payload 수락 완료가 아니라 재제출 가능 알림이다. Native completion queue를 NO_DATA까지 비운 뒤 재제출하는 경계는 Core/binding 소유다. Runner는 public awaitable 한 번만 기다리고 native 시도·WRITABLE 수를 `messages.sent`로 세지 않는다. 근거: `core/doc/spec/core/socket/README.ko.md:958`, `:967`, `:986`, `:1021`, `:1064`, `:1072`; drain owner `bindings/doc/spec/async-execution-model.ko.md:61`. |
| §23이 모든 completion의 ordinary HWM 격리를 요구 — `framework/doc/framework/common/perf/README.ko.md:1378`, `:1398`, `:1406` | **Topology 제한 필요.** RouteMesh ROUTER-ROUTER는 별도 Completion connection이다. ClientServer DEALER-ROUTER reply는 single FIFO의 앞선 DATA·HWM·PAUSED 뒤에서 늦어질 수 있다. Framework permit 우회는 Core가 completion으로 식별한 뒤의 규칙이다. 근거: E-QUEUE `:126`, E-CHANNEL `03-client-server-channel:351`; `framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.ko.md:60`. |
| §23 설정·permit 계산 — `framework/doc/framework/common/perf/README.ko.md:1340`, `:1392` | **대체로 유효.** 32/64/128/256 × effective processor와 manual 1..INT_MAX, reserved+queued 합계는 현재 계약과 같다. 단 vCPU만 바꿔 표를 기대하지 말고 constrained logical count·cpuset·quota·executor maximum의 최솟값을 기록해야 한다. 근거: `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:129`, `:133`, `:142`. |
| §23 reset이 counter·gauge만 요약 — `framework/doc/framework/common/perf/README.ko.md:1358`, `:1417` | **추가 필요.** Pause/resume threshold 기본 80/60, pressure state, current/cumulative pause duration, transition/config-failure metric이 빠졌다. Reset은 current pressure·pause를 보존한다. 근거: E-QUEUE `:253`; `framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.ko.md:68`, `:73`. |
| §4 startup ready와 endpoint 준비를 같은 것으로 볼 가능성 — `framework/doc/framework/common/perf/README.ko.md:88`, `:1049` | **명시 누락.** Host startup은 local ClientServer admission을 기다리지 않는다. ClientServer의 ready 후보 대기는 min(request timeout, 5초)이며 RouteMesh 후보 없음은 즉시 실패다. `/perf/ready`는 public topology status와 준비된 object/consumer를 확인해야 한다. 근거: `framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md:170`; D/J/K/N/C-STATUS. |
| §23 포화 부하에서도 연결이 유지된다고 해석할 여지 — `framework/doc/framework/common/perf/README.ko.md:1354`, `:1396` | **명시 누락.** Liveness는 5초/15초 고정, public tuning option이 없다. PAUSED는 liveness를 직접 바꾸지 않지만 control record의 지연은 deadline을 소진할 수 있다. Fanout이 15초 이상 포화돼 beacon을 못 받으면 not-ready가 되는 것은 계약상 결과다. 근거: `framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md:57`, `:63`, `:94`, `:181`; E-QUEUE `:137`. Timeout 확대나 manual reconnect로 이를 가리지 않는다. |
| §12/16의 ticks·Unix timestamp와 §4 duration — `framework/doc/framework/common/perf/README.ko.md:838`, `:1059`, `:1097` | **시간원 정의 누락.** Elapsed·deadline·retention은 monotonic, wall clock은 표기 전용이다. Process/host가 다른 `sentTicks`를 그대로 빼서 one-way latency로 계산할 수 없다. 근거: `framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md:70`, `doc/plan/c016-worklog/decisions.ko.md:1287`. §4.3의 단일 clock-domain 조건을 추가한다. |
| §4 settle·warmup 재연결과 durable operation — `framework/doc/framework/common/perf/README.ko.md:93`, `:321` | **경계 명시 필요.** Ordinary Actor/Spot payload는 자동 replay하지 않는다(E-ACTOR). OperationId를 가진 create/join 등의 durable lifecycle만 같은 ID·원래 deadline으로 replay한다. Caller cancellation·timeout으로 원격 업무를 되돌린다고 가정하지 않는다. 준비 operation이 끝난 뒤 warmup/reset하며, settle에서 재시도·새 CTS로 outcome을 바꾸지 않는다. 근거: `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:607`, `:635`, `:620`; E-SUBMIT `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:263`. |

**D-090…D-101의 적용 범위**

| 결정 / file:line | Perf에서 따를 결론 / 소유 절 |
|---|---|
| D-090·091 — `doc/plan/c016-worklog/decisions.ko.md:1205`, `:1235` | Submit 시점 pair가 끝나면 REQUEST는 원인 불문 NOT_CONNECTED terminal 1회다. 기다리던 WRITABLE token의 transient disconnect와 구분한다. Runner가 REJECT만 timeout으로 다시 분류하지 않는다. `core/doc/spec/core/socket/README.ko.md:1153`, `:1161`. |
| D-092 — `doc/plan/c016-worklog/decisions.ko.md:1247` | 물리 disconnect 진행을 위해 runner가 application recv 또는 두 번째 poller를 추가하지 않는다. Core/binding completion owner를 사용한다. `bindings/doc/spec/async-execution-model.ko.md:68`; Framework 소유권 E-QUEUE `:289`. |
| D-093 — `doc/plan/c016-worklog/decisions.ko.md:1256` | Durable replay 중단은 logical intent 제거+admitted peer 부재로 확정한 target lifecycle 종료다. 물리 단절만으로 중단하지 않는다. Deadline owner는 operation 하나다. `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:607`, `:635`. |
| D-094 — `doc/plan/c016-worklog/decisions.ko.md:1281` | 동일 endpoint 재연결 특례 없이 REJECT/HANDOVER 정책을 따른다. Perf에 “same endpoint이면 교체” 상태를 넣지 않는다. `core/doc/spec/core/socket/README.ko.md:165`, `:175`. |
| D-095 — `doc/plan/c016-worklog/decisions.ko.md:1287` | Monotonic duration만 사용한다. 위 clock 수정에 직접 해당한다. `framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md:70`. |
| D-096 — `doc/plan/c016-worklog/decisions.ko.md:1302` | 같은 peer를 hostname alias 두 개의 connect intent로 만들어 부하를 늘리지 않는다. Perf는 public logical target·connection 구성을 사용한다. `framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:315`; wire 소유 조항은 D-101이 지목한 Core ZMP §4.1(`doc/plan/c016-worklog/decisions.ko.md:1371`). |
| D-097 — `doc/plan/c016-worklog/decisions.ko.md:1310` | Cleanup shutdown seal 뒤 신규 admission을 기대하지 않는다. 이미 수락한 작업과 종료 결과만 관찰한다. 승격된 host shutdown 순서는 `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md` §14이며 승격 근거 `doc/plan/c016-worklog/decisions.ko.md:1375`. |
| D-098 — `doc/plan/c016-worklog/decisions.ko.md:1343` | Unbind/close 완료 뒤 rebind를 위해 임의 sleep을 추가하지 않는다. Draining descriptor도 publication terminal 뒤 시간 대기를 더하지 않는다. Closed intent는 늦은 READY로 재활성화하지 않는다. 현재 intent 절 `framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:319`; Core/host 승격 위치 `doc/plan/c016-worklog/decisions.ko.md:1372`. |
| D-099 — `doc/plan/c016-worklog/decisions.ko.md:1354`, `:1359` | STREAM fragment drain 관련 과거 실패·수정 기록이다. 별도 perf API나 sleep/retry 규칙의 근거가 아니다. Ingress는 현 공통 PACKET pull 계약을 따른다. `framework/doc/framework/common/spec/server/04-session/01-stream-session.ko.md:48`, `:139`. 이 보고서는 당시 실패의 현재 재현 여부를 판정하지 않는다. |
| D-100 — `doc/plan/c016-worklog/decisions.ko.md:1361` | Outbound intent 제거 시 endpoint 등록 해제는 runtime 한 곳이 결정한다. Runner가 peer generation 표나 재연결 보정을 소유하지 않는다. `framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:319`. |
| D-101 — `doc/plan/c016-worklog/decisions.ko.md:1366`, `:1378` | 결정 기록을 새 runtime 계약처럼 복사하지 않고 승격된 Actor §8.1, MeshNode §7.1, liveness §2를 인용한다. D-101 자체는 행동 변경이 아니다. |

현재 Framework submit 문서에는 “Core가 HWM 재시도를 소유한다”는 포괄 문구도 남아 있다.
0.17.0의 raw DONTWAIT caller는 WRITABLE drain 뒤 같은 record를 재제출하므로, runner 설명은
“Core/binding이 물리 admission 진행을 소유하고 Framework caller는 public call 하나를 기다린다”로
쓴다. Binding 내부가 모든 경우 Core의 payload 보관 재시도를 기다린다고 설명하지 않는다.
이는 인접 문서의 소유권 표현 정합성 문제이며 이 작업에서 수정하지 않는다.
근거: `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:138`,
`core/doc/spec/core/socket/README.ko.md:1064`, `:1072`, `bindings/doc/spec/async-execution-model.ko.md:68`.


D-096~D-101의 현재 본문 근거를 명시하면, 같은 RID의 동시 count-2 attempt는 wire에서 구분되지 않고
peer당 intent 하나로 수렴 조건을 만든다(`core/doc/spec/core/protocol/01-zmp.ko.md:202`). Shutdown은
새 application·peer admission을 먼저 닫고 Draining descriptor 게시의 terminal만 기다린다
(`framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:766`, `:771`).
`close`와 `unbind`의 endpoint 해제는 반환 전에 완료된다
(`core/doc/spec/core/socket/README.ko.md:627`, `:857`). 이들은 perf script의 임의 재연결·전파 sleep을
정당화하는 조건이 아니다. Durable replay는 typed transient transport 실패에 한정하며 protocol·encode·configuration
실패나 terminal envelope 수신 뒤에는 반복하지 않는다. 각 attempt는 남은 operation deadline을 사용한다
(`framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:611`, `:615`, `:620`, `:635`).
이는 §4 cleanup·preflight를 작성할 때 참조할 현재 계약이고, perf §4가 이미 반대 동작을 명시했다고
판정한 것은 아니다(`framework/doc/framework/common/perf/README.ko.md:81`).

## 3. Coverage gaps

“기능이 있으므로 하나 더”가 아니라 sample이 쓰는 경로가 기존 측정에서 빠지는 경우만 후보로 남겼다.
다음 추가 후보를 승인하지 않아도 기존 범위를 정정한 .NET runner부터 착수할 수 있다. 다만 그 결과를
Framework의 모든 interaction model을 덮은 baseline이라고 부를 수는 없다.
기존 질문의 범위 근거: `framework/doc/framework/common/perf/README.ko.md:21`, `:41`.

| 모델 | 판단과 측정 질문 | 소유 계약·public API·sample 근거 |
|---|---|---|
| **ClientServer Channel request/reply** | **추가 가치 높음.** ChannelName select-one, DEALER-ROUTER single connection과 readiness 비용을 독립 측정해야 한다. 현재 S2S Spot 시나리오의 “channel”만으로 topology가 확정되지 않는다. `channel-echo-only`를 두 topology로 명시하고 ClientServer 셀을 필수화하는 정도면 충분하다. | E-CHANNEL. `.NET` public `IZLinkRouteClient`와 Channel Client/Server 등록은 `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/03-configuration-topology.ko.md:222`, `:328`; 실제 소스 `framework/languages/dotnet/src/Zlink.Framework/Contracts/Configuration/Builders.cs:65`, `framework/languages/dotnet/src/Zlink.Framework/Contracts/Channels/RouteCalls.cs:19` 및 D-ROUTE의 Channel handler. Sample이 사용하는 ClientServer 목록 `framework/doc/framework/common/sample/README.ko.md:113`, `:114`; Java 실제 `framework/languages/java/samples/java/Bingo/Server/Session/src/main/java/systems/zlink/samples/bingo/server/session/sessions/handlers/AuthenticateSessionHandler.java:43`. |
| **Instance Spot** | **추가 가치 높음.** 모든 기존 Spot을 warmup 전에 생성하면 first-message activation·durable first request·cold/hot 차이를 못 잰다. 같은 ID 재사용의 warm path와 새 ID의 cold first-request 비용을 분리한 bounded 실험이 필요하다. Cold activation을 정상 steady echo에 섞지는 않는다. | `framework/doc/framework/common/spec/server/03-spot-actor/06-spot-address-messaging.ko.md:112`, `:128`; `framework/doc/framework/common/e2e/config-14-instance-spot.ko.md:49`, `:77`, `:574`, `:585`. `.InstanceSpot(type).InMesh(mesh)` public 선언 `framework/languages/dotnet/src/Zlink.Framework/Contracts/Spots/ZLinkSpot.cs:227`; 각 언어 D/J/K/N/C-ROUTE. 실제 Java `framework/languages/java/samples/java/ShoppingMall/Server/CommerceApi/src/main/java/systems/zlink/samples/shoppingmall/server/commerceapi/CommerceApiService.java:151`, C++ `framework/languages/cpp/samples/Bingo/Server/Api/Handlers/match_bingo_handler.hpp:38`. |
| **Spot address messaging** | **독립 추가 불필요.** §10.3/4를 current SpotId public call로 구현하면 이미 address resolve·routing을 통과한다. “Spot RID direct”를 유지해 address 모델을 우회하는 것이 문제다. | E-SPOT, D/J/K/N/C-ROUTE; `framework/doc/framework/common/e2e/config-2-spot-service.ko.md:70`; perf `framework/doc/framework/common/perf/README.ko.md:194`, `:632`. |
| **Relocation under load** | **후속 성능 실험 가치 있음, 최초 steady echo 범위 밖.** 같은 logical Actor/Spot의 이동 중 p99·완료율·interruption/resume 비용을 묻는다. 단순 정상 echo 수치의 baseline과 별도 결과로 관리한다. 장애·복구 조합 전체는 E2E에 남긴다. | `framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.ko.md:235`; public `RelocateAsync/relocate`는 `framework/languages/dotnet/src/Zlink.Framework/Contracts/Configuration/ZLinkDrainContracts.cs:172` 이후 lifecycle interface, `framework/languages/node/packages/framework/src/contracts/RouteMesh/RuntimeTopology.ts:150`; exact `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/08-location-maintenance.ko.md:219`. Samples `framework/doc/framework/common/sample/README.ko.md:51`, `:52`; config-14 in-flight relocate `framework/doc/framework/common/e2e/config-14-instance-spot.ko.md:445`. |
| **STREAM session bind/relay** | **정상 relay는 추가 불필요.** §10.1/2가 바로 local/remote bind 후 relay다. Bind 자체는 connect/setup latency로 별도 기록하면 된다. Binding 교체·relocation·disconnect의 조합을 throughput 시나리오로 복제하지 않는다. | E-SESSION, D/J/K/N/C-CS; `framework/doc/framework/common/perf/README.ko.md:89`, `:597`, `:615`; sample `framework/languages/cpp/samples/Bingo/Server/Session/Sessions/Handlers/authenticate_session_handler.hpp:59`. |
| **HTTP handler roundtrip (.NET/C++)** | **언어별 별도 측정 가치 있음, 5언어 공통 echo 필수 범위 밖.** Public HTTP host의 JSON binding·DI·dispatch 비용을 묻는다. Metrics HTTP endpoint의 비용과도 다르다. | C++ `framework/doc/framework/common/spec/server/languages/cpp/60-http-hosting.ko.md:13`, `:43`; public `framework/languages/cpp/framework/include/zlink/framework/contracts/http/http.hpp:251`, `:287`, `:292`. .NET 실제 `framework/languages/dotnet/samples/TicTacToe/Server/Api/Handlers/CreateGameHttpHandler.cs:9`; 기존 계획도 ASP.NET Core 비용 분리를 요구한다(`framework/doc/framework/perf/bindings/dotnet-framework-performance.ko.md:18`, `:39`). |
| **Spot Logical Multicast** | **추가 가치 있음.** Classic fanout은 PUB/SUB이고, Logical Multicast는 RouteMesh target 선택+node별 local Spot delivery다. 기존 pubsub 하나로 대표할 수 없다. 현재 sample의 milestone/border event 부하를 설명하려면 별도 결과가 필요하다. | E-SPOT `02-spot-messaging:260`, `:517`; public `.NET` `framework/languages/dotnet/src/Zlink.Framework/Contracts/Spots/ZLinkSpot.cs:197`의 outbound 계열, Node `framework/languages/node/packages/framework/src/contracts/Spots/Contracts.ts:87`; 실제 `.NET` `framework/languages/dotnet/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/BingoRoom.cs:263`, Java `framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/spots/ZoneSpot.java:307`. |

별도 “durable ordinary request replay throughput”은 추가하지 않는다. Actor/Spot 일반 message의
자동 replay는 current 계약이 아니며, lifecycle replay는 준비·이동 실험의 소유 경로다.
근거: E-ACTOR, `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:607`.

## 4. CLI / 결과 schema / endpoint의 구현 가능성

### 4.1 CLI와 trigger

| 항목 / perf 근거 | 판정과 최소 개정 |
|---|---|
| `--connections`, `--connect-concurrency`, client별 `--inflight` — `framework/doc/framework/common/perf/README.ko.md:108`, `:115`, `:116`, `:326` | CS에는 consumer가 있다. S2S·AC·PS는 “client는 trigger만”이므로 현재 정의 그대로라면 10,000 connector의 의미와 부하 분할 대상이 없다. CS는 physical connector 수, server-driven 셀은 server가 실행하는 logical operation stream 수를 구분해 기록한다. 이름만 같게 하고 단위를 숨기지 않는다. PS는 subscriber delivery를 기다리는 inflight ACK window가 없으므로 publisher-local admission 동시성만 정의한다. |
| Connector-only trigger·ActorCaller — `framework/doc/framework/common/perf/README.ko.md:185`, `:559`, `:762` | **그대로는 불가.** Session이 없는 ActorCaller의 RouteMesh/ClientServer socket에 stream connector가 직접 접속할 수 없다(E-SESSION/E-CHANNEL). 최소 개정은 CS만 connector client를 쓰고 server-driven 시나리오는 **application HTTP trigger**를 허용하는 것이다. Metrics endpoint로 trigger하지 않는 원칙은 유지할 수 있다. 현재 E2E도 sessionless Caller가 HTTP 요청을 받는다(`framework/doc/framework/common/e2e/config-9-to-actor-messaging.ko.md:30`). |
| `PerfTriggerRequest.batchSize`와 duration — `framework/doc/framework/common/perf/README.ko.md:840`, `:97` | batchSize 기본값·분할·실행 횟수·서버의 inflight가 없고 result는 client-visible와 server-local을 혼용한다. 최소한 role config가 measured duration·logical stream 수·inflight를 소유하고 trigger가 어느 window를 시작하는지, 완료 histogram이 어느 server에 쌓이는지 확정해야 한다. HTTP request 1건 또는 trigger batch 1개를 KOPS 1건으로 세면 안 된다. |
| `--spot-count`, `spotRids` — `framework/doc/framework/common/perf/README.ko.md:117`, `:194` | Consumer는 존재하나 식별자가 stale이다. User SpotId 목록이어야 하고 Object Server가 public manager로 준비해야 한다(E-SPOT). RID나 owner endpoint를 client가 지정하는 입력으로 만들지 않는다. Spot 배치 수뿐 아니라 execution mode·Actor 수와 mapping도 고정한다. |
| `--subscriber-count` — `framework/doc/framework/common/perf/README.ko.md:118`, `:189` | 그대로 구현 가능하다. N개 독립 process+N개 snapshot이다. Publisher의 stream 수와 subscriber 수를 혼동하지 않는다(E-FANOUT). |
| `--worker-pool-size` — `framework/doc/framework/common/perf/README.ko.md:120` | Public consumer가 5언어에 있다(D/J/K/N/C-TURN). MaxThreads만 같아도 minThreads·queue bound·idle timeout·executor 제한이 다르면 공정하지 않다. 나머지는 통일된 설정 또는 effective 값으로 함께 기록한다. |
| `--worker-task-millis` — `framework/doc/framework/common/perf/README.ko.md:119`, `:743` | Runner callback의 작업량 입력이다. Sleep을 CPU 작업과 동등하게 허용하지 않는다. Node는 callback closure를 직렬화하므로 임의 captured config를 읽을 수 없다(N-TURN actual declaration `:50`). Self-contained workload를 bootstrap에서 확정하고 작업 결과에 계측 timestamp/checksum을 반환하는 방법은 public callback으로 가능하다. 별도 worker thread나 내부 pool 접근은 불필요하다. |
| `--mode` — `framework/doc/framework/common/perf/README.ko.md:121` | Scenario가 이미 고정한 mode와 충돌할 때의 동작이 없다. 지원 mode만 수락하고 나머지는 preflight error로 고정한다. Explicit Yield 비교 mode를 추가한다면 언어 문법과 분리한 공통 값이어야 한다(E-TURN). |
| `--codec json` — `framework/doc/framework/common/perf/README.ko.md:122` | Standard 셀은 typed JSON 하나로 고정하는 것이 기존 목적에 맞는다. 임의 codec override를 받아도 수신 등록과 payload 규칙이 정의되지 않았다. 현재는 json 외 값을 거절하고 실제 serializer를 기록한다. Public message 원칙 `framework/doc/framework/common/perf/README.ko.md:8`, `:833`; typed 호출 근거 D/J/K/N/C-CS·ROUTE. |
| `--endpoint-config` — `framework/doc/framework/common/perf/README.ko.md:135`, `:185` | 하나의 appEndpoint에 stream ingress, mesh peer, fanout PUB, HTTP trigger가 혼재한다. 역할별 public trigger URL과 실제 transport endpoint를 구분하고 공통 registry 역할을 없앤다. Server 시작에 필요한 role config는 시작 **전** 생성하고, port 0 실제 endpoint manifest는 시작 뒤 만들 수 있다. Template 근거 `framework/doc/framework/common/sample/runner-templates/run_sample.template.sh:20`, `:98`. |
| Timeout/rate 설정과 §23 workload — `framework/doc/framework/common/perf/README.ko.md:1335`, `:1354` | §5에는 request deadline·send/send expiry·settle deadline·burst/rate·Core/queue profile의 consumer 입력이 없다. 일반 echo는 공통 default와 실제 값을 config에 기록하고, §23은 별도 workload manifest를 입력으로 받는 운영 실험임을 명시한다. 모든 설정을 일반 CLI option으로 늘릴 필요는 없다. Public 설정 소유자 `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:71`, `:113`. |

### 4.2 Metric별 public 측정 가능성

| Metric / perf 근거 | Public-only 가능 여부와 기록 방법 |
|---|---|
| Client/server completed·errors·RTT·process 자원 — `framework/doc/framework/common/perf/README.ko.md:875` | 가능. Call 시작/terminal과 application handler evidence, 언어·OS process API로 측정한다. Actor/Spot 내부 상태나 binding completion 횟수를 읽을 필요 없다. 오류는 E-ERROR로 통일한다. |
| `messagesPerSec`의 “실제 전송 message” — `framework/doc/framework/common/perf/README.ko.md:883`, `:919` | **Physical wire 전체 message/frame 수는 public Framework API로 공통 측정 불가.** 숨겨진 handshake·reply envelope·fragment·replay를 포함하면 계층을 넘는다. Application request/send/reply/event 수로 단위를 명시하고 내부 wire 수와 구분한다(E-SESSION, E-SUBMIT). Payload MiB/sec도 request 방향/echo 양방향/fanout delivery 중 무엇인지 고정한다. |
| `actor.localHandoff.*` — `framework/doc/framework/common/perf/README.ko.md:895` | Public send await까지 시간은 가능. **이름·정의는 틀림.** Remote Actor에서는 source transport admission이다. `actor.sourceAdmission.*`으로 고치고 왕복 histogram과 분리한다(E-SUBMIT). |
| `spot.mailboxDepth.max/mean` — `framework/doc/framework/common/perf/README.ko.md:897` | **5언어 공통 public API 없음.** Host의 reserved/queued/in-use aggregate는 존재하지만 Spot별 mailbox depth가 아니다. `null`+unsupported reason. Host aggregate를 같은 key에 넣지 않는다(E-METRIC, D/J/K/N/C-STATUS). |
| `spot.suspendedTurns/resumedTurns` — `framework/doc/framework/common/perf/README.ko.md:899` | 실제 turn suspension/reacquisition count는 public metric이 아니다(E-METRIC). Runner가 센 Yield 호출 수는 가능한 별도 application counter이지만 실제 suspension count와 같지 않다. 해당 key는 `null`로 두거나 명시적으로 application call count로 개명한다. |
| `spot.resumeLatency.*` — `framework/doc/framework/common/perf/README.ko.md:901` | **Remote reply가 재개 가능해진 내부 시점이 공개되지 않으므로 exact 측정 불가.** Yield 호출 전/후 시간 전체를 이 key로 넣지 않는다. `null` 처리한다(E-TURN, E-METRIC). |
| `spot.remoteRequestRtt.*` — `framework/doc/framework/common/perf/README.ko.md:903` | Public call 전후 경과 시간은 가능하지만 Yield gate 재획득·continuation 지연도 포함한다. `spot.remoteCallLatency.*`로 정의하거나 RTT가 이 전체 구간임을 명시한다. 순수 network RTT로 해석하지 않는다(E-TURN). |
| `worker.pool.queueDepth.*` — `framework/doc/framework/common/perf/README.ko.md:905` | **Public depth snapshot 없음.** Public worker options는 설정값뿐이다(D/J/K/N/C-TURN). Started/completed 차이는 running·transit도 포함하므로 실제 queue depth로 대체하지 않는다. `null`. |
| `worker.pool.queueWaitLatency`, `worker.taskLatency`, `worker.resumeLatency` — `framework/doc/framework/common/perf/README.ko.md:907` | Callback 시작·끝 timestamp를 worker 결과로 반환하고 caller 제출·재개 시각과 비교하는 **application 계측은 가능**하다. QueueWait에는 admission/dispatch overhead, Resume에는 결과 전달/gate 재획득을 포함한다고 정의한다. 공통 monotonic domain이 없는 조합은 `null`. Node는 shared mutable collector를 capture하지 않고 clone 가능한 결과를 반환한다(N-TURN `Contracts.ts:50`). |
| `fanout.deliveryLatency.*` — `framework/doc/framework/common/perf/README.ko.md:916` | Subscriber 자신의 clock과 publisher ticks는 일반적으로 직접 차감할 수 없다. Same-host 실행에서 공통 monotonic epoch/unit을 명시적으로 맞춘 경우만 측정한다. Multi-host에서는 clock alignment와 오차를 증명하지 못하면 `null`; delivery count/ratio는 계속 측정 가능하다. 시간원 소유 `framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md:70`; multi-host perf 허용 `framework/doc/framework/common/perf/README.ko.md:1288`. |
| §23 queued job wait p50/p95/p99·source fairness·permit leak — `framework/doc/framework/common/perf/README.ko.md:1365`, `:1395`, `:1417` | Aggregate status/reset은 가능. **Exact pre-receive→handler queue wait와 source별 permit handoff는 public hook으로 관찰할 수 없다.** Public callback outcome·sampled aggregate·계약 test 결과를 분리한다. Per-message 내부 tracing을 standard perf에 켜거나 sender RTT를 queue wait로 대체하지 않는다(E-QUEUE `:75`, E-METRIC `:82`). |

`null` 규칙은 지금도 있지만 “해당 시나리오가 지원하지 않으면”만 적혀 있다. 이 문구로는 Spot
시나리오에서 Spot metric을 못 얻는 상황을 처리하지 못한다. **“시나리오 비적용 또는 public 관측
미지원”을 구분해 null과 reason을 기록**하도록 개정해야 한다.
근거: `framework/doc/framework/common/perf/README.ko.md:921`, E-METRIC.

### 4.3 Result schema·측정 window

| 문제 / perf 근거 | 최소 정정 |
|---|---|
| 모든 셀이 같은 파일명 — `framework/doc/framework/common/perf/README.ko.md:131`, `:571`, `:946` | `run_perf.sh`가 여러 scenario×payload×mode를 한 run-id에서 실행하면 `result.json`, `client-0.json`, server 파일을 덮어쓴다. `<run-id>/<scenario>/<payload>/<variant>/`처럼 셀별 디렉터리를 고정하고 run root에는 index/summary만 둔다. Variant에는 Spot 수·subscriber 수처럼 결과를 구별하는 실제 설정을 포함한다. |
| Client 파일만 합산 — `framework/doc/framework/common/perf/README.ko.md:1032` | CS는 client가 metric owner, S2S/AC는 호출한 server, PS는 publisher+subscriber가 owner다. Trigger 수와 server echo 수를 합산하지 않는다. Subscriber별 원본 histogram과 count도 결과 입력에 포함한다. |
| Settle의 completion과 throughput 분모 — `framework/doc/framework/common/perf/README.ko.md:92`, `:93`, `:919` | Measured 동안 시작한 operation cohort와 측정 종료 전 완료한 operation을 구분한다. 권고: throughput은 window 안 완료 수, settle 완료는 별도 count/latency로 기록한다. Settle 시간은 별도 종료 bound를 가지며 새 operation을 시작하지 않는다. §13의 completed/expired/duplicate/unknown과 timeout 중복 계수 규칙도 닫아야 한다. |
| “동시 reset”·UnixMs — `framework/doc/framework/common/perf/README.ko.md:91`, `:1059`, `:1097` | 여러 process에 HTTP reset을 원자적으로 동시에 적용할 수 없다. Warmup을 정지·drain하고 모든 role의 같은 resetSeq 응답을 확인한 뒤 measured 시작 barrier를 연다. UnixMs는 관찰 timestamp, measuredSeconds는 각 owner의 monotonic 경과다. Host capacity reset은 D/J/K/N/C-STATUS를 쓰고 app counter reset과 구분한다. |
| Histogram으로 모든 percentile·mean/max 복원 — `framework/doc/framework/common/perf/README.ko.md:926`, `:940`, `:1032` | Bucket 합산은 맞다. 다만 고정 0.1..1024ms bucket은 quantized estimate이며 overflow는 상한이 없다. Percentile 추정 규칙·overflow 표현, 정확한 mean/max용 sum/count/max를 공통 정의한다. `.NET`·C++의 빠른 셀에서 p50=0.1ms라고 정확값처럼 보고하지 않는다. |
| `messages.completed=null`인 PS의 KOPS — `framework/doc/framework/common/perf/README.ko.md:919`, `:923` | PS의 echo KOPS·echo latency는 `null`. Publish admission ops/sec와 subscriber delivery ops/sec를 각각 계산한다. `deliveryRatio` 분모는 measured window의 성공한 publish 수, 분자는 같은 measured sequence 범위의 unique 수신이다. 분모 0은 `null`과 failed/invalid run으로 기록한다. |
| Pub sequence와 multi-client trigger — `framework/doc/framework/common/perf/README.ko.md:849` | Publisher 1개가 run 내 sequence를 단독 발급하고 warmup/measured 범위를 분리한다. 여러 trigger client가 sequence를 독립 발급하면 unique-count 근거가 깨진다. New wire protocol은 필요 없고 기존 sequence 필드의 소유자만 정하면 된다. |
| DTO field type·ticks unit·byte encoding 미정 — `framework/doc/framework/common/perf/README.ko.md:836` | Shared contract에서 field type·JSON representation·ticks 단위를 확정한다. Payload 논리 byte 수와 serialized message 크기를 구분한다. Node status의 bigint를 JSON number로 조용히 줄이지 않는다. 실제 bigint public 값은 `framework/languages/node/packages/framework/src/contracts/RouteMesh/RuntimeTopology.ts:91`, `:120`; actor/ref 값 형식의 공통 예는 `framework/doc/framework/common/spec/server/03-spot-actor/06-spot-address-messaging.ko.md:99`. |
| Success criteria와 zero-error — `framework/doc/framework/common/perf/README.ko.md:319`, `:809`, `:1405` | 99% connect 기준은 연결 준비의 유효성이지 모든 workload의 성공 판정이 아니다. PS loss 허용과 §23 “drop=0”를 같은 gate로 적용하면 충돌한다. §23 lossless pass condition은 그 보장을 가진 대상 workload에 한정한다. 실패가 있는 run은 원인·count를 보존하고 숫자만 성공 baseline으로 채택하지 않는다(E-FANOUT). |

### 4.4 Metrics endpoint·port·process 격리

`/perf/reset`, `/perf/stats`, `/perf/ready`는 **application이 소유하는 admin HTTP endpoint로
구현 가능**하다. Framework가 동일 URL을 내장 public API로 제공할 필요는 없다. .NET host와
C++ public HTTP builder, JVM/Node application HTTP 서버를 사용하고 내부 runtime collector를
꺼내지 않으면 된다. HTTP가 어려운 경우 admin channel을 허용한다는 현 규정도 유지할 수 있지만,
`metricsUrl`과 runner HTTP 고정 호출도 그 transport를 표현하도록 맞춰야 한다.
근거: `framework/doc/framework/common/perf/README.ko.md:1041`, `:377`, `:1117`;
C++ `framework/languages/cpp/framework/include/zlink/framework/contracts/http/http.hpp:287`;
sessionless HTTP application endpoint 예 `framework/doc/framework/common/e2e/config-9-to-actor-messaging.ko.md:30`.

| 격리 항목 | 판단 / 근거 |
|---|---|
| 기존 process 일괄 정리 | Perf §9의 “기존 perf server process 정리”는 scope가 없다(`framework/doc/framework/common/perf/README.ko.md:576`). 이름/prefix kill로 해석하면 sample 격리와 충돌한다. 이번 run이 소유한 PID/process handle과 정확한 container ID만 정리한다. Template `framework/doc/framework/common/sample/runner-templates/run_sample.template.sh:60`, `:63`. |
| 포트 예시 21001·31001 | 고정값을 실제 기본값으로 쓰면 C++ sample application 20100..21999, C++ E2E application 30100..31999와 겹친다. Perf 예시 `framework/doc/framework/common/perf/README.ko.md:145`; 구간 `framework/doc/framework/common/sample/README.ko.md:429`, `framework/languages/cpp/e2e/redis-common.sh:5`. 예시임을 명시하고 실제 값은 port 0 bind 후 조회하거나 검증된 예약을 쓴다. Sample/e2e 구간을 perf가 독점하는 새 범위로 가정하지 않는다. |
| 검사기 적용 범위 | `scripts/verify-framework-runner-isolation.py:66`, `:129`, `:142`의 inventory는 sample/E2E다. 미래 perf runner가 자동으로 검증된다고 볼 수 없다. Perf 등록은 후속 구현 범위로 따로 추가해야 한다. 현재 이 검사기는 실행하지 않았다. |
| Java/Kotlin build 충돌 | Perf §17.3의 공유 build root는 맞다. Build 시 sample/E2E와 같은 build-only lock 규칙을 적용하고 process 실행 전에 해제한다. `framework/doc/framework/common/sample/runner-templates/run_sample.template.sh:41`, `framework/doc/framework/common/sample/README.ko.md:440`. |
| Server config·role 분리 | 역할별 executable은 현 perf와 template이 일치한다(`framework/doc/framework/common/perf/README.ko.md:338`; template `:135`). Role은 생성된 config 파일 하나를 받고 standalone client는 CLI를 받는다. Env로 app endpoint/timeout을 추가 전달하는 두 번째 설정 owner를 만들지 않는다(template `:6`, `:98`). |
| `/perf/ready`의 의미 | Host/listener가 시작됐다는 것만으로 ready=true를 반환하면 안 된다. 필요한 Channel 후보, object Ready, fanout subscriber 최초 marker 수신을 public status/evidence로 확인한다(E-CHANNEL, E-FANOUT). 이 검증은 measured path 밖에 둔다(`framework/doc/framework/common/perf/README.ko.md:814`, `:1117`). |

### 4.5 시나리오별 Docker/Redis 필요성

**Location Store 필요성과 Docker 필요성은 다른 결정이다.** Object Client/Server에는 명시적
Location Store가 필요하다. Manual peer endpoint만으로 local object runtime을 대신할 수 없다.
Docker는 Framework API 전제가 아니라 재현 가능한 runner 격리 방식이다. Sample 규칙을 perf에도
채택할 경우 Store가 필요한 각 실행은 전용 Docker Redis를 만들고 host Redis fallback을 허용하지
않는다고 공통 perf에 명시해야 한다.
근거: `framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:121`, `:278`;
`framework/doc/framework/common/sample/README.ko.md:415`; template
`framework/doc/framework/common/sample/runner-templates/redis-common.template.sh:68`.

| Scenario | Location Store | 표준 runner 의존성 권고 / 이유 |
|---|---|---|
| `cs-local-session-actor-echo` | 필요 | 전용 Docker Redis. Local Actor도 Object Server의 global Actor authority를 사용한다(E-ACTOR, E-SESSION). TicTacToe도 Redis를 사용한다(`framework/doc/framework/common/sample/tictactoe/README.ko.md:117`). |
| `cs-remote-session-actor-echo` | 필요 | Session Object Client와 Actor Object Server가 같은 run Store를 사용한다. Perf의 “필요하면 Registry”를 Store로 바꾼다(`framework/doc/framework/common/perf/README.ko.md:620`; E2E config-2 `:28`). |
| `s2s-channel-to-spot-request-echo`, `s2s-channel-to-spot-send-send-echo` | 필요 | Global User Spot 및 caller Object Client 때문에 필요하다(E-SPOT, MeshNode `:121`). |
| `s2s-spot-to-channel-request-echo`, `s2s-spot-to-channel-send-send-echo` | 필요 | User Spot host 때문에 필요하다. 순수 ChannelServer까지 object factory를 가질 필요는 없다(E-SPOT, MeshNode `:117`). |
| `spot-async-request-echo`, `spot-await-contention` | 필요 | User Spot placement/authority 때문에 필요하다. RemoteEcho는 순수 ChannelServer로 충분하다(E-SPOT). |
| `spot-no-await-echo`, `spot-worker-offload-echo` | 필요 | RemoteEcho가 없어도 User Spot Object Server는 남는다(E-SPOT, MeshNode `:121`). |
| `actor-no-bind-request-echo`, `actor-no-bind-send-send-echo` | 필요 | 현 spec의 Redis 의존성은 맞다(`framework/doc/framework/common/perf/README.ko.md:769`, `:787`; E-ACTOR). |
| `pubsub-fanout-echo` | 표준 automatic 구성에서 필요 | 현재 perf는 Redis를 요구한다(`framework/doc/framework/common/perf/README.ko.md:804`). Classic fanout manual endpoint 계약도 존재하지만 standard 셀을 임의로 manual로 바꾸지 않는다(`framework/doc/framework/common/e2e/config-3-pubsub.ko.md:20`, `:29`). |
| `session-echo-only` | 자체로 불필요 | Actor dispatch/object role·automatic service discovery를 추가하지 않은 순수 STREAM session이면 없다(E-SESSION). 다른 application 기능 때문에 필요해지는 경우와 구분한다. |
| `channel-echo-only` | topology에 따라 다름 | Manual ClientServer는 불필요, automatic discovery는 필요(`framework/doc/framework/common/spec/server/02-channel-transport/03-client-server-channel.ko.md:246`). Object role 없는 manual RouteMesh도 object Store를 이유로 요구하지 않는다(MeshNode `:117`). |
| `spot-local-echo`, baseline `spot-no-await-echo` | 필요 | 같은 process여도 Spot Object Server다(E-SPOT). |
| `connector-echo-only` | 판정 대상 아님 | 현재 정의를 public-only로 구현할 수 없어 standard 착수 선행 항목에서 제거해야 한다(E-SESSION). |

Relocation Store는 Location Store와 별도 계약이다. 정상 echo에 cross-node join/maintenance를
의도치 않게 넣지 않으면 relocation workload를 추가할 이유가 없다. PreserveStateWith relocation을
포함하기로 하면 별도 provider/key namespace를 기록한다. 동일 Redis deployment를 쓰는 것과
두 Store 계약을 합치는 것은 다르다.
근거: `framework/doc/framework/common/sample/README.ko.md:51`, `:59`;
`framework/doc/framework/common/sample/zoneworld/README.ko.md:604`.

## 5. 공통 이름과 언어별 계획의 충돌

**유지할 이름의 소유자는 `common/perf/README.ko.md`다.** 언어 계획의 표를 별도 공통 계약처럼
유지하면 이름·payload matrix·runner 위치가 다시 갈라진다. 네 계획은 현재도 옛
`framework/doc/framework/perf/README.ko.md`를 공통 정책으로 링크한다.
근거: `framework/doc/framework/common/perf/README.ko.md:533`, `:551`;
`framework/doc/framework/perf/bindings/dotnet-framework-performance.ko.md:3`,
`framework/doc/framework/perf/bindings/cpp-framework-performance.ko.md:25`,
`framework/doc/framework/perf/bindings/java-framework-performance.ko.md:22`,
`framework/doc/framework/perf/bindings/node-framework-performance.ko.md:21`.

아래는 단순 rename 가능한 것과 **동등한 새 시나리오가 없는 것**을 구분한 전체 대조다.
CS 약어가 같다고 `client_server_request_reply`를 session→actor echo로 개명하면 측정 경로가 바뀐다.
옛 뜻의 근거는 `framework/doc/framework/perf/README.ko.md:90`의 표다.

| 옛 이름 | 유지할 공통 이름 / 처리 | 판단 |
|---|---|---|
| `client_server_request_reply` | §3에서 권고한 ClientServer `channel-echo-only` 셀 | 13개와 일대일 rename 없음. Session/Actor CS와 다르다. 옛 근거 `framework/doc/framework/perf/README.ko.md:93`; 현 E-CHANNEL. |
| `client_server_send` | ClientServer channel baseline의 one-way 측정 | send/send echo와 동등하지 않다. completion/delivery 정의 후 별도 variant로만 남긴다. 옛 `:92`, 새 perf `:854`. |
| `fanout_publish_1`, `fanout_publish_n` | `pubsub-fanout-echo` + `subscriberCount=1/N` | rename+parameter 통합 가능. 기존 단방향 fanout 의미 유지. 옛 `:94`, `:95`; 새 `framework/doc/framework/common/perf/README.ko.md:794`. |
| `dealer_mesh_request_reply` | 제거 또는 별도 역사적 결과 label | 현재 RouteMesh/ClientServer 중 어느 계약을 뜻하는지 옛 계획만으로 동등 매핑 불가. “dealer mesh”를 새 public topology로 만들지 않는다. 옛 `framework/doc/framework/perf/README.ko.md:96`; 현 E-CHANNEL. |
| `route_mesh_request_reply` | `channel-echo-only`의 RouteMesh request 셀 | Spot dispatch를 추가하지 않는 조건으로 baseline 대응. 옛 `:98`; 새 `framework/doc/framework/common/perf/README.ko.md:825`. |
| `route_mesh_send` | `channel-echo-only`의 RouteMesh send 셀 | one-way이며 request/send-send echo와 직접 rename 불가. 옛 `framework/doc/framework/perf/README.ko.md:97`. |
| `stream_request_reply` | `session-echo-only` | Actor 없는 STREAM echo라는 조건에서 대응. 옛 `:100`; 새 `framework/doc/framework/common/perf/README.ko.md:824`; E-SESSION. |
| `stream_send` | 현재 필수 13개에 일대일 이름 없음 | 단방향 submit/delivery를 request echo로 rename하지 않는다. 옛 `framework/doc/framework/perf/README.ko.md:99`. |
| `bound_session_send` | 현재 필수 13개에 일대일 이름 없음 | bound push는 CS request reply와 다른 완료다. 필요하면 session baseline 확장으로 따로 승인. 옛 `:101`; `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/07-bound-stream-session.ko.md:10`. |
| `stream_actor_relay` | `cs-local-session-actor-echo` / `cs-remote-session-actor-echo` | 배치와 request completion을 확정한 뒤 두 이름으로 분할 대응. 단방향 relay-only 수치를 echo로 재표기하지 않는다. 옛 `framework/doc/framework/perf/README.ko.md:102`; 새 perf `:595`, `:613`. |
| `spot_to_spot_send`, `spot_to_spot_request_reply` | 일대일 대응 없음 | Channel→Spot 또는 Spot→Channel로 개명하면 caller/target owner가 바뀐다. 기존 결과는 별도 과거 label, 신규 범위는 공통 spec의 요청에 따라 결정. 옛 `framework/doc/framework/perf/README.ko.md:103`, `:104`. |
| `spot_to_router_egress` | `s2s-spot-to-channel-request-echo` / `s2s-spot-to-channel-send-send-echo` | 옛 전송 방식이 무엇인지 확정한 후 방향명 rename+mode 분리. 옛 `:105`; 새 perf `:657`, `:670`. |
| `router_to_spot_ingress` | `s2s-channel-to-spot-request-echo` / `s2s-channel-to-spot-send-send-echo` | 위와 동일. 옛 `framework/doc/framework/perf/README.ko.md:106`; 새 perf `:630`, `:643`. |
| `http_handler_roundtrip` | .NET/C++ 별도 HTTP 측정 이름으로 유지 가능 | 5언어 필수 공통 13개에 억지 rename하지 않는다. 옛 `framework/doc/framework/perf/README.ko.md:107`; §3의 HTTP 범위 판단. |

옛 계획의 64B/1KB/4KB/64KB full matrix, fake backend 및 `run_benchmarks.sh` 위치도 현 공통
standard와 별도 취급해야 한다. 공통 표준은 1024/4096, 실제 process와 public API, `run_perf.sh` /
`run_single.sh`다. Micro/fake backend 결과는 별도 measurement layer로 남길 수 있지만 common perf
완료 수로 세지 않는다.
근거: `framework/doc/framework/perf/README.ko.md:109`, `:59`;
`framework/doc/framework/perf/bindings/dotnet-framework-performance.ko.md:76`;
`framework/doc/framework/common/perf/README.ko.md:37`, `:65`, `:75`, `:568`.

## 6. 단순화: 13 → 11

**권고 병합:** `s2s-spot-to-channel-request-echo`를 남기고 `spot-async-request-echo`,
`spot-await-contention`을 그 scenario의 비교 셀로 옮긴다. 같은 Spot handler가 같은 remote
Channel request를 보내고 reply를 기다리는 hot path다. `Async`와 `Yield`, Spot 수 1과 16의
차이는 독립된 프로그램이 아니라 같은 workload의 두 입력이다. External trigger RTT는 보조값으로
두고 공통 완료 단위를 **Spot handler 안에서 시작한 remote call completion**으로 맞춘다.
근거: `framework/doc/framework/common/perf/README.ko.md:659`, `:685`, `:701`, `:717`; E-TURN.

| 병합 뒤 실행 셀 | 남기는 질문 |
|---|---|
| 일반 terminal, SpotId 1 / 16 | Remote channel request의 기본 비용과 owner 분산 효과는 얼마인가? |
| Yield terminal, SpotId 1 / 16 | 같은 부하·remote server에서 shared gate를 반납하면 처리량·tail latency·다른 callback 진행이 어떻게 달라지는가? |

**비교한 대안:** 이름 13개를 그대로 두고 문구만 고치면 기존 이름 대응은 쉽지만, 같은 remote
request loop·Spot 배치·완료 집계를 세 곳에서 맞춰야 한다. 위 병합은 그 규칙 소유자를 하나로
줄인다. 통합이 부담되면 §10.7/8만 합쳐 12개로 줄일 수도 있지만 §10.5와 겹치는 loop가 남는다.
새 runtime API, helper 계층 또는 성능 정책은 필요 없다.
근거: 시나리오별 파일을 요구하는 `framework/doc/framework/common/perf/README.ko.md:396`,
같은 조건 비교를 요구하는 `:717`, `:1299`.

그 외는 합치지 않는다. Local/remote session actor는 transport hop 유무, channel→Spot과
Spot→channel은 수신 dispatcher 및 gate owner, request와 send/send는 completion owner,
worker는 local CPU scheduler, no-bind actor는 session route와 다른 ID resolve, fanout은 N개
subscriber delivery라는 서로 다른 질문이 있다.
근거: `framework/doc/framework/common/perf/README.ko.md:21`, `:43`, `:655`, `:753`, `:773`, `:808`;
E-SESSION·E-ACTOR·E-TURN·E-FANOUT.

Baseline은 별도로 정리한다. `connector-echo-only`는 standard 착수 선행 항목에서 제거하고,
`spot-local-echo`는 §10.9 `spot-no-await-echo`의 local 실행 셀에 대한 alias/reference로만 둔다.
Baseline 중복을 표준 시나리오 감소 수에 더하지 않았다.
근거: `framework/doc/framework/common/perf/README.ko.md:821`, `:1240`, E-SESSION.

수정 전/후 규칙 수: **표준 scenario 13→11, Spot→Channel request loop의 규격 소유자 3→1**.
Terminal 2종과 Spot 수 2종의 비교 질문은 모두 유지한다.

## 7. 권고 개정 목록과 .NET 착수 판정

아래 문장은 supervisor가 perf spec에 넣을 **최소 replacement 초안**이다. 공통 runtime spec을
개정하는 안이 아니다. “Runner 변경”은 앞으로 작성할 runner의 동작·출력에 영향을 주는지 뜻한다.
현재 runner가 없으므로 기존 perf 결과의 migration을 구현할 필요는 없다
(`doc/plan/c016-worklog/framework-perf-runner-inventory.md:4`).

1. **규격 소유자와 이름.** 위치: `framework/doc/framework/common/perf/README.ko.md:7`, `:533`.
   초안: “표준 scenario 이름·payload matrix·CLI·결과 형식은 이 문서가 소유한다. 언어별 계획은
   구현 도구와 runtime metadata만 보완하며 다른 시나리오 이름을 표준 이름으로 사용하지 않는다.”
   Runner 변경: **예**, accepted scenario/config 집합. 근거: §5.

2. **Spot 식별자·필수 Store.** 위치: `framework/doc/framework/common/perf/README.ko.md:117`, `:194`, `:353`.
   초안: “Spot 대상은 global SpotId이며 `spotIds`는 public manager로 준비한 User Spot ID 목록이다.
   Object Client/Server를 사용하는 모든 시나리오는 공식 Location Store를 등록한다. Registry라는
   공통 server role은 사용하지 않는다.” Runner 변경: **예**, bootstrap·manifest. 근거: E-SPOT,
   `framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:121`.

3. **Turn 의미.** 위치: `framework/doc/framework/common/perf/README.ko.md:24`, `:685`, `:701`, `:738`.
   초안: “일반 비동기 terminal은 현재 owner turn을 유지한다. Shared gate 반납 비용은 `SpotWide`
   User Spot에서 명시적 Yield terminal로 측정한다. 일반 terminal과 Yield의 비교는 같은 payload,
   in-flight, Spot 수와 remote server에서 수행한다.” Runner 변경: **예**, handler terminal.
   근거: E-TURN. §21·§22의 ‘자동 turn’ 문구도 같은 의미로 바꾼다(`:1299`, `:1314`).

4. **겹치는 시나리오 병합.** 위치: `framework/doc/framework/common/perf/README.ko.md:657`, `:683`, `:699`.
   초안: “`s2s-spot-to-channel-request-echo`는 일반 terminal/Yield와 SpotId 수 1/16의 비교 셀을
   실행한다. 각 셀은 Spot handler 내부 remote call completion을 측정하며 독립 결과 파일을 갖는다.”
   Runner 변경: **예**, 13→11·시나리오 파일·목록·mode. §8.4·§18·§22도 목록만 함께 맞춘다.
   근거: §6. API 동작을 바꾸는 안은 아니다.

5. **ActorId와 source admission.** 위치: `framework/doc/framework/common/perf/README.ko.md:762`, `:781`, `:895`.
   초안: “ActorCaller는 session을 만들거나 bind하지 않고 global ActorId로 direct send/request를
   호출한다. Send terminal은 current Ready authority resolve와 source-local admission 완료이며,
   remote mailbox 또는 handler 완료가 아니다. `actor.sourceAdmission.*`와 왕복 완료를 분리한다.”
   Runner 변경: **예**, target DTO·metric key. 근거: E-ACTOR·E-SUBMIT.

6. **Public 오류 분류.** 위치: `framework/doc/framework/common/perf/README.ko.md:754`, `:774`, `:894`.
   초안: “Framework 오류는 공통 ErrorKind를 언어별 enum에서 정규화하여 기록한다. 내부 route·worker
   reason을 별도 필수 kind로 요구하지 않는다. Harness validation·correlation expiry는 Framework
   오류와 구분한다.” Runner 변경: **예**, error schema. 근거: E-ERROR.

7. **한 logical operation과 backpressure.** 위치: `framework/doc/framework/common/perf/README.ko.md:326`, `:854`.
   초안: “In-flight는 public call 시작부터 첫 terminal 또는 echo correlation 종료까지의 logical
   operation 수다. Source admission 대기를 포함하며 native DONTWAIT 시도·wait token·WRITABLE을
   별도 operation으로 세지 않는다. Runner는 physical retry·completion drain·reconnect를 구현하지 않는다.”
   Runner 변경: **예**, loop·counter. 근거: E-SUBMIT, Core socket `:963`, `:1068`의 위 근거.

8. **Trigger와 부하 단위.** 위치: `framework/doc/framework/common/perf/README.ko.md:108`, `:185`, `:559`, `:840`.
   초안: “CS는 stream connector로 직접 부하를 만든다. S2S·ActorCaller·Publisher는 별도 application
   trigger endpoint를 호출하는 standalone client로 시작하고 server가 public API loop를 실행한다.
   Trigger 전송은 HTTP를 허용하며 metrics endpoint를 사용하지 않는다. Server logical stream 수,
   in-flight와 duration은 role config에 기록하고 connector 수와 별도 단위로 보고한다.”
   Runner 변경: **예**, 필수 선행 결정. 근거: §4.1, E2E config-9 `:30`.

9. **Send/send return 주소와 correlation.** 위치: `framework/doc/framework/common/perf/README.ko.md:645`, `:672`, `:781`, `:836`.
   초안: “Send/send echo는 request/reply 지원 여부와 관계없이 harness correlationId를 사용한다.
   Return Channel은 지정 caller 하나만 Server로 등록하고, 여러 source Spot을 쓰면 application
   metadata 또는 DTO에 return SpotId를 명시한다. 원격 send terminal과 correlation 완료는 따로 센다.”
   Runner 변경: **예**, return handler/DTO. 근거: E-CHANNEL·E-SUBMIT·D-ROUTE context.

10. **Public metric 제한.** 위치: `framework/doc/framework/common/perf/README.ko.md:883`, `:897`, `:905`, `:921`, `:1417`.
    초안: “Metric은 application 계측, public host aggregate와 provider 계기로만 얻는다. Spot별
    mailbox/실제 suspended turn/reply-ready 시각/worker queue depth/exact host queue wait를
    public으로 얻지 못하면 null과 unsupported reason을 기록한다. Application message count와
    callback 구간 latency를 내부 wire 수·queue 지연으로 바꿔 부르지 않는다.”
    Runner 변경: **예**, 불가능한 필수 항목 제거. 근거: E-METRIC, §4.2.

11. **Worker workload.** 위치: `framework/doc/framework/common/perf/README.ko.md:119`, `:743`, `:751`.
    초안: “CPU worker는 고정한 self-contained CPU workload를 사용하고 sleep을 대체 부하로
    사용하지 않는다. Pool max/min threads·queue bound·idle timeout과 실제 callback 실행 시간을
    기록한다. Callback의 시작·종료 계측은 반환값으로 전달할 수 있다.” Runner 변경: **예**.
    근거: E-TURN, N-TURN actual callback 계약.

12. **Window·reset·settle.** 위치: `framework/doc/framework/common/perf/README.ko.md:91`, `:93`, `:919`, `:1051`.
    초안: “Warmup 제출을 정지하고 잔여 작업을 정리한 뒤 모든 role의 resetSeq 응답을 확인하여
    measured barrier를 연다. Measured 종료 뒤 새 operation을 제출하지 않으며 window 안 완료와
    settle 완료를 분리한다. Throughput 분모는 monotonic measuredSeconds이고 settle에는 별도
    유한 종료 bound를 적용한다.” Runner 변경: **예**, 필수 선행 결정. 근거: §4.3, E-SUBMIT.

13. **Clock·DTO 형식.** 위치: `framework/doc/framework/common/perf/README.ko.md:836`, `:916`, `:1097`.
    초안: “Elapsed·deadline·latency는 monotonic clock을 사용하고 Unix timestamp는 표기에만 쓴다.
    Shared DTO의 정수·payload JSON 형식과 ticks 단위를 고정한다. Cross-process one-way latency는
    검증된 공통 clock domain에서만 계산하고 그렇지 않으면 null과 이유를 기록한다.”
    Runner 변경: **예**. 근거: liveness `framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md:70`, N-STATUS.

14. **결과 셀·집계 owner·histogram.** 위치: `framework/doc/framework/common/perf/README.ko.md:946`, `:1032`, `:926`.
    초안: “각 scenario/payload/mode/배치 셀은 별도 디렉터리에 원본 결과를 저장한다. CS는 client,
    S2S/AC는 호출 server, PS는 publisher와 각 subscriber의 count·histogram을 집계한다. Percentile은
    공통 bucket의 추정 규칙과 overflow 정책으로 계산하며 mean/max는 sum/count/max를 사용한다.”
    Runner 변경: **예**, schema. 근거: §4.3.

15. **Fanout 단위.** 위치: `framework/doc/framework/common/perf/README.ko.md:794`, `:849`, `:919`.
    초안: “이 시나리오는 Classic fanout public client를 사용한다. Publisher가 sequence를 단독
    발급하며 measured publish 성공 수와 같은 sequence 범위의 subscriber unique delivery를 센다.
    Echo KOPS는 null이고 publish admission 및 subscriber delivery throughput을 별도로 기록한다.”
    Runner 변경: **예**. 근거: E-FANOUT·E-SPOT, §4.3.


   함께 교체할 문장: “Classic Fanout은 packet name으로 typed handler를 선택하며 subscriber별
   transport topic filter를 제공하지 않는다. `config-3`의 PS-A2는 packet name별 handler 선택을
   검증한다.” 위치: `framework/doc/framework/common/perf/README.ko.md:811`; 현재 근거:
   `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:693`,
   `framework/doc/framework/common/e2e/config-3-pubsub.ko.md:71`. **Runner 변경: 없음** — 현재 perf는
   이 기능을 측정 범위에서 제외하지만, 존재하지 않는 기능을 후속 runner가 전제로 삼지 않게 한다.

16. **§23 적용 topology와 current pressure.** 위치: `framework/doc/framework/common/perf/README.ko.md:1340`, `:1378`, `:1398`, `:1405`.
    초안: “Profile matrix는 실제 effective processor count로 판정한다. Pause/resume threshold와
    current/epoch pressure metric을 기록한다. Ordinary HWM과 분리된 reply 진행은 RouteMesh에
    적용하며 ClientServer의 single FIFO/HWM 지연은 계약대로 판정한다. Zero-drop은 lossless
    workload에만 요구한다. 내부 permit handoff 불변식은 contract test의 책임으로 남긴다.”
    Runner 변경: **예**, tuning 판정. 근거: E-QUEUE·E-METRIC·E-FANOUT.

17. **격리·readiness·cleanup.** 위치: `framework/doc/framework/common/perf/README.ko.md:87`, `:576`, `:1049`, `:1278`.
    초안: “각 run은 검증된 port 또는 runtime port 0, 독립 로그·config와 필요시 전용 Docker Redis를
    사용한다. Cleanup은 이번 run의 PID와 container ID만 대상으로 한다. Ready는 public topology와
    application 준비 evidence로 판단하고 liveness 5초/15초 및 ClientServer ready 대기 cap을 변경하지
    않는다. Java/Kotlin은 기존 build-only lock을 공유한다.” Runner 변경: **예**. 근거: §4.4·4.5.

18. **Baseline과 구현 순서.** 위치: `framework/doc/framework/common/perf/README.ko.md:821`, `:1240`, `:1310`.
    초안: “최초 검증 baseline은 public session-echo-only다. Connector-only는 이 common runner의
    필수 선행 항목이 아니다. Spot local baseline은 spot-no-await-echo의 local 셀을 참조한다.
    언어별 계획과 완료 목록은 합의한 표준 시나리오 및 측정 가능 metric을 따른다.”
    Runner 변경: **예**, 불가능한 착수 선행 조건 제거. 근거: E-SESSION, §6.

§3의 신규 coverage는 위 필수 오류 수정과 분리한 **범위 선택**이다. 우선순위는 ClientServer
baseline 필수화 → Instance cold/hot → Logical Multicast → relocation 부하 → .NET/C++ HTTP 별도
결과다. 이를 승인하면 해당 질문·결과 단위를 공통 문서에 먼저 추가한다. 과거 plan의 이름만 가져와
runner부터 구현하지 않는다. 근거: §3과 공통 perf의 역할 경계
`framework/doc/framework/common/perf/README.ko.md:8`, `:18`.

### .NET 착수 순서

**GO 조건:** 위 2~18의 필수 입력·완료·출력 정의와 §5의 이름 소유권을 supervisor가 확정하면,
새 Framework public API 없이 .NET echo runner부터 구현할 수 있다. Source public 호출은 §1의
D-*가 제공한다. 아래 순서는 구현 순서이며 이 검토에서 실행한 목록이 아니다.

1. 공통 typed JSON DTO·cell schema·monotonic timing·error mapping·metrics endpoint·격리 runner.
2. `session-echo-only` — connector와 결과 집계부터 검증.
3. `cs-local-session-actor-echo` — explicit create/bind 후 local relay.
4. `cs-remote-session-actor-echo` — 별도 Session/Actor process.
5. `channel-echo-only` — RouteMesh와 ClientServer 셀을 구분. ClientServer를 필수 coverage로 승인하면 이때 완료.
6. `spot-no-await-echo` — `spot-local-echo` baseline 참조를 함께 해결.
7. `s2s-channel-to-spot-request-echo`.
8. `s2s-channel-to-spot-send-send-echo`.
9. 병합한 `s2s-spot-to-channel-request-echo` — 일반 terminal/Yield × SpotId 1/16.
10. `s2s-spot-to-channel-send-send-echo`.
11. `spot-worker-offload-echo` — 고정 CPU workload·명시 terminal.
12. `actor-no-bind-request-echo`.
13. `actor-no-bind-send-send-echo`.
14. `pubsub-fanout-echo` — subscriber별 최초 수신 barrier와 원본 결과.
15. 승인된 경우 Instance cold/hot, Logical Multicast, relocation-under-load, HTTP 별도 실험.
16. §23 tuning은 workload manifest·실효 CPU·memory 제한이 정해진 뒤 별도 실행 계획으로 진행.

2와 5는 baseline이고 6은 표준 시나리오와 baseline을 겸한다. 따라서 3·4·6~14가 표준 **11개**다.
최종 검증은 실제 구현 뒤 담당자가 수행해야 하며, 이번 읽기 전용 진단을 test 통과 근거로 사용하지 않는다.
기존 순서와 비교한 근거: `framework/doc/framework/common/perf/README.ko.md:1237`, `:1240`, `:1251`, `:1253`.

## 읽은 범위

검토 도중 공유 workspace의 문서 commit이 갱신되어 변경된 계약 조항을 추가 대조했다.
최종 인용 기준 HEAD는 `7627284944fe2df46f7f7a3b3a00795b22d5b927`이다. 이 작업에서는 git 상태를 변경하지 않았다.


- 루트 `AGENTS.md` §2·§3·§5를 먼저 읽고 `doc/AGENTS.md`, `framework/AGENTS.md`,
  `framework/doc/AGENTS.md`, `doc/principal/documentation/documentation-principles.ko.md`를 확인했다.
  Runtime 수정은 없으므로 runtime 구현 승인 단계는 진행하지 않았다.
- `framework/doc/framework/common/perf/README.ko.md`의 목표·실행·CLI·구조·13 scenario·5 baseline·
  message·fairness·metrics·schema·endpoint·언어 위치·순서·완료·HWM 절을 읽었다. 근거 위치는 위에
  열거했으며 핵심 범위는 `:7`, `:81`, `:100`, `:200`, `:440`, `:593`, `:816`, `:831`, `:854`,
  `:869`, `:944`, `:1039`, `:1121`, `:1232`, `:1306`, `:1324`다.
- 현 공통 계약은 §1.3 E-*에 적은 execution, channel, Spot/Actor, STREAM/session,
  observability 절과 liveness·framework API·error model을 관련 경계 중심으로 읽었다.
  Language interface와 real public 선언은 D/C/J/K/N-*의 file:line을 대조했다. Internal 구현을
  public API의 근거로 대체하지 않았다. Node worker callback의 격리 방식은 public comment와
  worker source를 함께 확인했다(`framework/languages/node/packages/framework/src/contracts/Spots/Contracts.ts:50`).
- Common sample README의 topology·terminator·Redis isolation, TicTacToe·Bingo·ZoneWorld·
  SupportChat·event 문서의 관련 절과 각 언어 sample의 실제 호출 지점을 검색·대조했다.
  대표 근거는 §1.4 및 `framework/doc/framework/common/sample/README.ko.md:113`, `:279`, `:409`다.
- Common E2E config-2·3·8·9·14의 관련 interaction/expectation을 읽었다. 범위 근거는
  `framework/doc/framework/common/e2e/config-2-spot-service.ko.md:5`,
  `framework/doc/framework/common/e2e/config-3-pubsub.ko.md:5`,
  `framework/doc/framework/common/e2e/config-8-execution-turn.ko.md:5`,
  `framework/doc/framework/common/e2e/config-9-to-actor-messaging.ko.md:5`,
  `framework/doc/framework/common/e2e/config-14-instance-spot.ko.md:15`다. 모든 E2E 구현을 실행하거나
  전체 inventory를 재검증한 것은 아니다.
- 네 언어 perf 계획과 그 상위 구 정책의 scenario·measurement layer·payload·runner 위치를
  읽었다(§5). Runner 부재는 `doc/plan/c016-worklog/framework-perf-runner-inventory.md:4`를
  사용했다. `doc/plan/c016-worklog/decisions.ko.md`는 D-090~D-101과 승격 내용을 확인했다(§2).
- `scripts/verify-framework-runner-isolation.py:66`의 port/lock/inventory와 sample
  `runner-templates`의 config·Docker·cleanup·build lock 규칙을 읽었다(§4.4). 검사기는 실행하지 않았다.
- Core DONTWAIT·REQUEST 종료 의미는 `core/doc/spec/core/socket/README.ko.md:958`, `:1064`,
  `:1153`, binding completion owner는 `bindings/doc/spec/async-execution-model.ko.md:61`,
  `bindings/doc/spec/async-coroutine-policy.ko.md:54`를 읽었다. Core/binding 파일은 수정하지 않았다.

## BLOCKERS

1. **현재 perf 본문 그대로 착수 불가:** 자동 turn 반납, ActorRef 메시징·handoff 의미,
   내부 전용 오류/metric 요구를 먼저 고쳐야 한다. Perf 근거 `framework/doc/framework/common/perf/README.ko.md:685`,
   `:762`, `:782`, `:894`, `:897`; 소유 계약 E-TURN·E-ACTOR·E-SUBMIT·E-ERROR·E-METRIC.
2. **측정 규격 결정 필요:** Session 없는 caller의 trigger transport, server-driven 부하 단위와
   metric owner, window/settle 포함 범위, 셀별 파일명, clock/histogram 규칙이 닫혀야 5언어가 같은
   결과를 만들 수 있다. Perf 근거 `framework/doc/framework/common/perf/README.ko.md:108`, `:185`,
   `:840`, `:919`, `:946`, `:1032`; 최소 개정 §7 항목 8·12~15.
3. **.NET 이후 C++ public 선언 정합성 확인:** Session relay의 exact interface와 source가 다르다.
   `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md:286` 대
   `framework/languages/cpp/framework/include/zlink/framework/contracts/actors/actor.hpp:874`.
   실제 public 호출은 있어 .NET 착수를 막지는 않지만 C++의 계약 parity를 보장한 것으로 기록할 수 없다.
4. **인접 계약·sample 표현 차이:** 일반 submit의 HWM retry 소유권 문구(§2 마지막 단락)와
   Node Bingo의 submit/Yield 사용 차이(§1.4)는 해당 소유자에게 전달할 사항이다. Perf에서
   그 차이를 보정하거나 sample을 수정하지 않는다.
5. **실행 검증 결과 없음:** 빌드·test·benchmark 금지 조건을 준수했다. 따라서 이번 보고서에서
   신규 runtime 실패를 확정하지 않았고, D-099의 과거 실패를 현재 실패로 재분류하지 않았다.
   기존 runner 부재 근거는 `doc/plan/c016-worklog/framework-perf-runner-inventory.md:4`다.

필수 개정을 반영하면 **.NET canonical 구현 착수 GO**. 신규 coverage 확장은 별도 범위 결정이며,
구현 완료·성능 baseline 확정은 실제 runner와 후속 검증이 끝난 뒤 판정한다.
