<!-- framework-adapter-nav:start -->
[문서 목록](../README.ko.md) | [이전: Runtime Lifecycle](../../common/internals/README.ko.md) | [다음: Backend Dependency Policy](backend-dependency-policy.ko.md)
<!-- framework-adapter-nav:end -->

[Node.js 묶음](../README.ko.md) | [Runtime Lifecycle](../../common/internals/README.ko.md) | [Backend Policy](backend-dependency-policy.ko.md) | [공통 E2E](../../common/e2e/README.ko.md)

# ZLink Framework Node.js Regression Test Matrix

> 회귀 항목의 기준은 `framework/languages/node`의 구현과 Node 테스트
> (`node:test` 기반 `*.test.js`, `test(...)`)다. dotnet 회귀 matrix 는 parity
> 비교용 선택 참고로만 본다.

## 1. 목적

이 문서는 구현이 바뀌더라도 무엇이 깨지면 회귀로 보는지를 테스트 항목 단위로
정리한다. 공개 동작은 책임 spec이 소유하고, 이 문서는 검증과 release gate만
소유한다.

dotnet 테스트 프로젝트의 계약·단위·E2E 구분은 비교할 때 참고한다. Node는
`node:test` 기반의 자체 디렉토리와 테스트 이름을 사용하므로 파일 구조나 테스트
이름을 그대로 복제하지 않는다.

| dotnet 프로젝트 | node 테스트 패키지 | 계층 매핑 |
|------|------|------|
| `Zlink.Framework.ContractTests` | `@zlink-systems/framework` contract 테스트(`test/contract/**/*.test.js`) | `unit` 계약 표면 + 일부 `contract` |
| `Zlink.Framework.UnitTests` | unit 테스트(`test/unit/**/*.test.js`) | `unit` |
| `Zlink.Framework.E2ETests` | e2e 테스트(`test/e2e/**/*.test.js`) | `integration-single-process` + `integration-multi-process` |

> 공통 공개 계약이 같은 동작을 요구하는지는 비교하되, Node에 존재하지 않는
> Registry runtime이나 테스트 묶음을 비교 대상 언어에서 그대로 가져오지 않는다.

## 2. CI 계층

회귀 테스트는 다음 세 계층으로 나누어 둔다.

| 계층 | 목적 | 예시 |
|------|------|------|
| `unit` | registration validation, dispatch lookup, option parsing | 중복 등록, module options validation |
| `integration-single-process` | 같은 호스트(Node process) 안에서 runtime 조합이 정상 동작하는지 확인 | channel request/send, location runtime, monitoring attach |
| `integration-multi-process` | 실제 topology[^topology]와 reconnect 동작 확인 | 원격 registry query, discovery 변화, spot peer 변화 |

## 3. 최소 CI 매트릭스

| 항목 | 기준 |
|------|------|
| Node.js 런타임 | `node20`(LTS), `node22`(현행) |
| 플랫폼 ABI[^abi] | `win-x64`, `win-arm64`, `linux-x64`, `linux-arm64`, `darwin-x64`, `darwin-arm64` |
| test mode | development(소스 `*.ts`), production(번들·`dist`) |

최소 지원 런타임이 `node20` 이므로, 회귀 테스트도 위 두 런타임 버전을 함께
돌려야 한다.

- 현재 저장소의 기본 빌드는 `node20` 단일 런타임이다.
- `node22` 는 회귀 matrix 보고용 다중 런타임 빌드에서 추가로 컴파일·실행하는
  형태로 다룬다.

한편 저장소의 `@zlink-systems/zlink`(Node 바인딩) prebuilt native artifact 조합과
CI workflow 가 만들어 내는 native artifact 조합은 위 여섯 플랫폼 ABI 를 기준으로
한다. framework CI gate[^ci-gate] 도 같은 범위를 기본으로 본다.

즉 Node framework 회귀 테스트는 특정 OS 하나만 대표로 돌리고 끝내지 않는다.
현재 계획 기준으로 반드시 통과해야 하는 플랫폼은 다음과 같다.

- Windows x64
- Windows ARM64
- Linux x64
- Linux ARM64
- macOS x64
- macOS ARM64

## 3.1 Node Binding Parity Regression 항목

> framework 는 `@zlink-systems/zlink` public API 위에만 올라간다. dotnet
> `Runtime/Backend/DotNet/` 이 `bindings/dotnet` public surface 를 쓰는 것처럼,
> Node backend adapter 도 binding internal/native 경로를 직접 우회하지 않는다.

| 항목 | 계층 | 통과 기준 |
|------|------|-----------|
| binding public API parity | `unit` | channel, Spot, stream, monitoring, session relay와 bound session이 binding public API만으로 동작한다 |
| framework public-api-only import guard | `unit` | framework runtime/adapter package가 binding internal path, native addon symbol, generated private helper를 import하지 않는다 |
| session relay public API smoke | `integration-single-process` | stream session relay가 binding public API만으로 동작한다(별도 attach 없음) |
| bound session public API smoke | `integration-single-process` | bound session send/disconnect가 binding public API만으로 동작한다 |
| registry query public API smoke | `integration-single-process` | registry query client wrapper가 binding public API만 호출한다 |
| socket monitor public API smoke | `integration-single-process` | socket monitoring source가 binding public API만 호출한다 |
| native artifact freshness guard | `unit` | native addon 산출물이 source보다 오래되면 framework smoke가 실패한다 |

## 4. Channel Regression 항목

> dotnet `ContractTests/Channels`, `ContractTests/Handlers`,
> `ContractTests/Configuration`, `E2ETests/Channels` 미러. Node의
> `addRouteMesh(meshName)` / `channelName(channelName)` 등록과
> `ZLinkChannelClient` / `ZLinkRouteClient` / `ZLinkFanoutClient` 표면을 검증한다.

| 항목 | 계층 | 통과 기준 |
|------|------|-----------|
| duplicate MeshName 등록 (`addRouteMesh`) | `unit` | startup validation 예외 |
| 빈 MeshName 또는 빈 ROUTER endpoint | `unit` | startup validation 예외 |
| `addRouteMesh(...).listen(...)` + `channelName(...)` | `integration-single-process` | 명시한 MeshNode와 논리 channel에서 request/send 성공 |
| `peerConnections().connect(...)` | `integration-single-process` | 수동 peer 연결을 사용한 request/send 성공 |
| `.addFanoutChannel(...)` + `.enableSubscriber(endpoint)` | `integration-single-process` | manual 기반 subscribe 성공 |
| publisher 역할에 bind endpoint 없음 | `unit` | startup validation 예외 |
| publisher 전용 channel | `integration-single-process` | publish submit 성공 |
| subscriber discovery attach | `integration-multi-process` | 원격 publish 수신 |
| handler group mapping | `unit` | handler decorator 등록만으로는 전역 dispatch 대상이 되지 않고, channel 의 `handlerGroups: ['...']`로 매핑한 그룹의 handler만 해당 채널에서 dispatch된다 |
| handler exposure 없는 server channel | `unit` | scan 된 handler 가 있어도 `addHandlerGroup(...)` 또는 `add*Handler(...)`가 없으면 application handler 가 자동 노출되지 않는다 |
| handler exposure 없는 server channel validation | `unit` | handler exposure 없는 server channel 은 application handler를 열지 않는다 |
| fanout subscriber handler exposure | `unit` | `addPublishHandler(...)` 또는 handler group으로 등록한 publish handler만 subscriber dispatch 대상으로 노출된다 |
| typed handler registration | `unit` | channel 의 `add*Handler(...)`로 직접 등록한 handler 는 group mapping 없이도 해당 channel 에 노출된다 |
| channel type handler compatibility | `unit` | RouteMesh는 send/request, fanout subscriber는 publish handler만 허용하며 각 channel 역할과 맞지 않는 등록은 거부한다 |
| incompatible handler group mapping | `unit` | channel type 과 맞지 않는 handler 가 group 안에 섞이면 일부만 제외하지 않고 startup validation 오류로 실패한다 |
| route mesh handler group mapping | `integration-single-process` | route mesh channel 의 `addHandlerGroup(...)`은 route send/request handler group 을 실제 routed dispatch 대상으로 노출한다 |
| route mesh packet dispatcher | `integration-single-process` | route mesh `ROUTER` 로 들어온 routed send/request packet 을 handler 로 dispatch 하고 request reply/error 를 돌려주며 빈 probe frame 은 application handler 로 넘기지 않는다 |
| DI route channel inbound handler dispatch | `integration-single-process` | `ZLinkModule.forRoot(...)` route channel 의 `sendHandlers`/`requestHandlers`가 runtime host 시작 후 host-owned `ROUTER` receive loop 에 연결되어 routed send/request 를 처리한다 |
| DI route client host transport | `integration-single-process` | `ZLinkModule.forRoot(...)`가 노출한 `ZLinkRouteClient`가 framework runtime host 시작 이후 host-owned ROUTER transport로 target node RID에 routed send/request/reply를 수행한다 |
| Spot route transport 전용 membership | `integration-single-process` | MeshNode에 Spot factory와 논리 channel만 등록해도 Spot route transport로 동작하며, 등록하지 않은 application handler는 노출하지 않는다 |
| 같은 channel server에 handler 중복 | `unit` | 같은 `kind + packetName` handler가 둘 이상이면 startup validation 예외 |
| 다른 channel server에 같은 packet handler | `integration-single-process` | 같은 `kind + packetName`을 서로 다른 channel에 매핑해도 각 채널이 독립적으로 dispatch된다 |
| 같은 그룹을 여러 채널에 매핑 | `integration-single-process` | 같은 `zlinkRequestHandler('api', ...)` group 을 두 채널에 `handlerGroups`로 노출해도 채널마다 dispatch namespace가 독립이다 |
| `addHandlerGroup`이 가리키는 그룹 없음 | `unit` | 매핑한 그룹에 handler가 하나도 없으면 startup validation 오류 |
| event handler group mapping | `unit` | publish handler group mapping 은 subscriber handler registration 표면이 생긴 뒤 추가한다 |
| HTTP(REST controller) handler에서 `ZLinkChannelClient` 사용 | `integration-single-process` | route handler와 동일한 NestJS DI[^di] 컨테이너에서 정상 동작 |
| DI channel client host transport | `integration-single-process` | `ZLinkModule.forRoot(...)`가 노출한 `ZLinkChannelClient`가 framework runtime host 시작 이후 host-owned DEALER transport로 manual channel request/reply를 수행한다 |
| channel handler에서 `ZLinkChannelClient` 사용 | `integration-single-process` | 일반 request handler가 같은 DI 컨테이너의 `ZLinkChannelClient`로 다른 channel 에 request 하고 reply 를 받는다 |
| channel handler에서 fanout publish | `integration-single-process` | 일반 request handler가 같은 DI 컨테이너의 `ZLinkFanoutClient`로 fanout event 를 publish 하고 subscriber handler가 수신한다 |
| send one-way submit backpressure[^backpressure] | `integration-single-process` | `submit()`은 즉시 한 번 admission을 시도하고, capacity가 부족하면 send timeout까지 기다린 결과를 반환한다 |
| publish one-way submit backpressure | `integration-single-process` | send와 같은 admission 계약을 사용하며 원격 수신 완료를 기다리지 않는다 |
| request submit/reply timeout 분리 | `integration-single-process` | request packet의 submit 지연은 submitter timeout 정책으로, reply 대기는 `timeout(...)`으로 판정 |
| pending request 정리 | `unit` | submit 실패, timeout, cancellation(`AbortSignal`), runtime stop이 일어날 때 request sequence가 pending map에서 제거된다 |
| ready callback batch drain | `integration-single-process` | socket이 ready된 뒤 pending send/publish를 batch로 처리하고, 같은 frame을 중복 전송하지 않는다 |
| channel wire multipart[^wire-multipart] | `integration-single-process` | 서버 간 channel send/request/reply가 `header`와 `payload`를 별도 message part로 보내고, handler dispatch는 header part만 보고 packet을 고른다 |
| publish wire multipart | `integration-single-process` | `PUB/SUB` publish도 framework header와 payload를 별도 part로 유지하고, subscriber handler에는 typed payload만 전달된다 |

one-way send와 publish의 `submit()`은 bounded admission 결과를 비동기로 반환하지만 원격 수신이나
handler 실행 완료는 기다리지 않는다.

## 4.1 Dispatch Error Observer Regression 항목

| ID | 계층 | 테스트 위치 | 통과 기준 |
|----|------|-------------|-----------|
| DERR-001, DERR-007, DERR-011, DERR-014 | `unit` | `test/contract/channel-client.test.js` | channel request handler 없음은 error reply와 observer event, channel send handler 없음은 drop과 observer event, observer 예외는 원래 dispatch 결과를 깨지 않음 |
| DERR-002, DERR-008 | `unit`, `integration-single-process` | `test/contract/channel-client.test.js` | route request handler 없음은 error reply, route send handler 없음은 drop과 observer event로 끝남 |
| DERR-003, DERR-004, DERR-009, DERR-010, DERR-016 | `unit`, `integration-single-process` | `test/contract/spot-manager.test.js`, `test/contract/actor-manager.test.js` | SPOT route, subscription, actor dispatch 실패가 request면 error reply 또는 caller-visible rejection, one-way면 drop과 observer event로 끝남 |
| DERR-005, DERR-006, DERR-013, DERR-015 | `unit`, `integration-single-process` | `test/contract/channel-client.test.js`, `test/contract/spot-manager.test.js` | decode 실패와 handler 예외는 error reply 또는 관측 가능한 drop으로 끝나며, observer 미등록 시에도 기본 로그와 counter가 남음 |

## 4.2 DI Capability Regression 항목

> dotnet `ContractTests/Configuration/ConnectionAndConfigContracts`,
> `UnitTests/Configuration/Registration` 미러. NestJS provider token 노출 규칙
> 역할별 provider 노출 조건을 검증한다.

| 항목 | 계층 | 통과 기준 |
|------|------|-----------|
| actor factory without MeshNode | `unit` | actor factory만 등록하면 startup validation 예외 |
| actor manager without MeshNode | `unit` | MeshNode 없는 구성에서는 `ZLinkActorManager` provider token이 DI에 없다 |
| actor manager with MeshNode only | `unit` | MeshNode만 있고 actor factory가 없으면 `ZLinkActorManager`가 DI에 없다 |
| actor manager with MeshNode and actor factory | `unit` | MeshNode와 actor factory가 모두 있으면 `ZLinkActorManager`가 DI에 등록된다 |
| Spot service without MeshNode | `unit` | MeshNode 없는 구성에서는 `ZLinkSpotManager`가 DI에 없다 |
| Spot service with MeshNode | `unit` | MeshNode가 있으면 Spot service가 DI에 등록된다 |
| Spot publisher without publisher 역할 | `unit` | MeshNode가 있어도 publisher 역할이 없으면 Spot publisher service는 DI에 없다 |
| Spot publisher with publisher 역할 | `unit` | Spot publisher 역할이 있으면 `ZLinkSpotPublisherClient` 가 DI 에 등록된다 |
| bound session factory registration | `unit` | `ZLinkBoundSessionFactory` 는 framework runtime 과 함께 등록된다 |
| SpotRef resolver without MeshNode | `unit` | location store를 등록한 서버는 MeshNode 없이 `ZLinkSpotRefResolver`를 등록할 수 있다 |
| Spot outbound with resolver only | `unit` | SpotRef resolver만 있고 MeshNode가 없으면 `ZLinkSpotOutbound`는 DI에 없다 |
| route channel missing at call time | `unit` | `ZLinkRouteClient` 호출 시 route channel 이 없으면 `ZLinkConfigurationException` |
| channel client missing at call time | `unit` | `ZLinkChannelClient` 호출 시 channel client 역할이 없으면 `ZLinkConfigurationException` |

## 5. Spot Regression 항목

> dotnet `ContractTests/Spots`, `ContractTests/Actors`, `E2ETests/Spot` 미러.
> `ZLinkSpotManager`, `ZLinkActorManager`, Entry Spot, bound session 표면을
> 검증한다.

| 항목 | 계층 | 통과 기준 |
|------|------|-----------|
| duplicate Spot factory type | `unit` | startup validation 예외 |
| duplicate `addEntrySpot(EntrySpotClass)` | `unit` | 같은 MeshNode 안에서 Entry Spot[^entry-spot] registry를 중복 등록하면 startup validation 예외 |
| `addRouteMesh(meshName)` + `addEntrySpot(...)` | `integration-single-process` | MeshNode 빌더에서 Entry Spot과 Spot factory 등록을 한 번에 끝낸다 |
| location store 없이 local MeshNode 등록 | `integration-single-process` | 수동 peer 연결만 사용하는 MeshNode runtime을 시작한다 |
| `create(spotType)` | `integration-single-process` | `spotId`, `Created` 상태가 일관되게 유지된다 |
| `create(spotType)` empty create payload | `integration-single-process` | payload 없는 생성도 빈 `ZLinkMessage`로 `ZLinkSpot.onCreate(...)`를 한 번 호출한다 |
| `create(spotType, request)` payload | `integration-single-process` | create request `ZLinkMessage`가 `ZLinkSpot.onCreate(...)`로 한 번 전달된다 |
| `getOrCreate(spotType, spotRid, request)` existing | `integration-single-process` | 같은 `spotId`가 이미 ready 상태면 `Existing`이고 새 `request`는 `onCreate(...)`로 전달되지 않는다 |
| `getOrCreate(...)` concurrent create payload | `integration-single-process` | 같은 `spotId` 동시 생성에서는 첫 생성 요청의 `ZLinkMessage`만 `onCreate(...)`로 전달되고 callback은 한 번만 실행된다 |
| `getOrCreate(spotType, spotRid)` same type | `integration-single-process` | 같은 `spotId`를 같은 Spot 타입으로 다시 확보하면 기존 spot을 반환하고 새 `onCreate(...)`를 호출하지 않는다 |
| spot create lifecycle failure | `integration-single-process` | `onCreate(...)` 또는 `onInitialize(...)` 실패는 `SpotCreateFailed`로 전파되고 failed entry는 제거되어 다음 생성 요청이 재시도할 수 있다 |
| `find(...)`, `list(...)` | `integration-single-process` | manager 조회 결과가 일관된다 |
| `configure()` handler registration | `integration-single-process` | `context.handlers.addPacket(...)`, `context.handlers.addHandler(...)`, `context.handlers.addSubscribe(...)` 등의 spot-local 등록이 descriptor에 반영된다 |
| Entry Spot actor handler decorator registration | `integration-single-process` | `zlinkEntrySpotActorRequestHandler(...)` decorator 로 등록한 actor packet handler 가 대상 Entry Spot registry에 반영된다 |
| user Spot actor handler decorator registration | `integration-single-process` | `zlinkSpotActorRequestHandler(...)` decorator 로 등록한 actor packet handler 가 대상 user Spot registry에 반영된다 |
| Entry Spot packet callback concurrency | `integration-single-process` | Entry Spot 일반 packet handler는 user Spot과 같은 등록 표면을 쓰지만 Entry Spot 전체 실행 줄에 직렬화되지 않는다 |
| `onInitialize(...)` handler resolve | `integration-single-process` | spot마다 분리된 DI scope가 정상 동작한다 |
| `onClosing(...)` 정상 close callback | `integration-single-process` | `close(...)` 호출 시 spot 실행 문맥에서 한 번 호출된다 |
| local spot publish | `integration-single-process` | subscriber가 정상 수신한다 |
| SPOT timer metadata | `integration-single-process` | timer handler가 callback 번호, 예정/시작 시각, 지연, skip metadata를 받는다 |
| SPOT timer overrun policy | `integration-single-process` | `SkipLateTicks`, `CatchUpBounded`, `DelayNextTick` 정책이 각각 skip, bounded catch-up, fixed-delay 의미를 지킨다 |
| SPOT timer exception policy | `integration-single-process` | handler 예외가 monitoring event로 기록되고, `stopOnUnhandledException`이 켜진 timer는 중단된다 |
| Entry Spot actor packet dispatch | `contract` | `entry spot actor packets use actor mailboxes without entry-wide serial dispatch` 와 `entry spot actor handler yield fails immediately instead of timing out` 으로 대상 actor mailbox, 같은 actor 순서, 서로 다른 actor 진행, yield 계약 오류를 검증한다 |
| Entry Spot timer execution context | `integration-single-process` | Entry Spot timer는 Entry Spot lifecycle callback, request continuation과 같은 Entry Spot 실행 queue에서 처리되고, 같은 timer callback도 겹쳐 실행하지 않는다. actor packet은 대상 actor mailbox에서 처리한다 |
| SPOT timer cancel | `integration-single-process` | `cancel()` 뒤 managed timer loop가 추가 callback을 실행하지 않는다 |
| outbound 전용 외부 publish client | `integration-multi-process` | target SPOT[^spot] channel에 publish가 성공한다 |
| Spot route channel acceptance | `unit` | fanout/dealer mesh/ambiguous/missing router/missing peer source 구성을 startup validation에서 거부한다 |
| MeshNode 수동 peer 연결 | `integration-single-process` | `peerConnections().connect(...)`의 endpoint가 RouteMesh peer admission에 적용된다 |
| Spot route transport | `integration-single-process` | MeshNode의 ROUTER 경로가 target Spot handle을 사용해 routed send/request를 전달한다 |
| Spot egress target peer 선택 | `integration-single-process` | 수동 연결과 location metadata의 owner RID를 사용해 target MeshNode peer를 선택한다 |
| Spot route egress 역할 validation | `unit` | routed Spot egress는 RouteMesh에서만 사용할 수 있고 fanout 구성에서는 startup validation 오류다 |
| spot close 후 scope 정리 | `integration-single-process` | 이후 callback이 발생하지 않고 dispose도 정상 완료된다 |
| actor join 이후 dispatch 문맥 | `integration-single-process` | `ZLinkSpotContext.addHandler(...)`로 등록한 actor handler가 join된 `Spot` 실행 문맥에서 실행된다 |
| Entry Spot actor dispatch serialization | `integration-single-process` | Entry Spot actor packet이 actor별 입력 순서를 보존한 뒤 Entry Spot 실행 queue에서 순서대로 처리된다 |
| local actor mailbox dispatch | `integration-single-process` | user Spot에 들어가지 않은 actor packet도 actor별 mailbox 순서를 따른다 |
| user Spot actor dispatch serialization | `integration-single-process` | 같은 user Spot 안의 여러 actor packet이 Spot 실행 queue에서 순서대로 처리되어 Spot 상태가 보호된다 |
| runtime task exception observation | `unit` | detached runtime task와 fire-and-forget handler에서 발생한 예외가 unhandled rejection으로 묻히지 않고 runtime error sink 또는 logger로 관찰된다 |
| execution queue cancellation semantics | `unit` | queue enqueue/wait cancellation이 이미 queue에 들어간 work item의 순서를 깨거나 중간에 제거하지 않는다 |
| Spot handle route 경로 | `integration-single-process` | routed Spot 호출은 `SpotHandle`의 MeshName, owner RID, generation을 검증한 뒤 routed message를 보낸다 |
| actor manager 생성 중복/타입 충돌 | `integration-single-process` | `ZLinkActorManager.create(...)` 중복 생성은 `ActorAlreadyExists`, `getOrCreate(...)` actor type 충돌은 `ActorTypeMismatch` 로 실패한다 |
| local actor bind 생성 금지 | `integration-single-process` | `bind(...)` 는 local actor 가 없을 때 factory 를 호출하지 않고 `ActorRouteNotFound` 로 실패한다 |
| session actor bind: fallback 없이 logical actor handle 등록 | `integration-single-process` | `bind(...)` 는 application resolver fallback 없이 logical actor handle 을 등록한다 |
| remote actor dispatch 생성 금지 | `integration-single-process` | routed actor dispatch 수신 경로는 local actor 가 없을 때 factory 를 호출하지 않고 dispatch 를 실패시킨다 |
| session actor relay bridge | `integration-single-process` | `bind(...)` 와 `ZLinkSessionActor.relay(...)` 가 public session 표면에서 동작한다 |
| session actor explicit disconnect notification | `contract`, `integration-single-process` | session disconnect 는 bound actor 전체에 자동 전파되지 않고, `notifyDisconnected(...)` 또는 runtime 명시 호출 시 현재 Spot actor disconnected handler 가 호출된다 |
| session actor dispatch ordering | `integration-single-process` | stream session에서 actor로 relay된 packet이 actor별 순서를 보장하고, 현재 actor 위치에 맞는 handler 실행 경로로 넘어간다 |
| actor dispatch location after mailbox wait | `integration-single-process` | 같은 actor의 앞선 packet이 join을 끝낸 뒤, 대기 중이던 다음 packet이 이전 위치가 아니라 새 user Spot 위치로 dispatch된다 |
| session actor dispatch wire multipart | `integration-single-process` | Session 서버와 Play 서버 사이의 actor dispatch가 route header, actor metadata, stream header, payload를 별도 part로 유지하고, payload를 JSON envelope 안의 `Buffer`로 재직렬화하지 않는다 |
| session actor reconnect 재사용 | `integration-single-process` | 같은 actor id가 새 stream session에서 다시 bind되면 기존 actor 인스턴스와 spot membership을 유지하고, session binding token[^binding-token]만 갱신된다 |
| session actor binding rollback | `integration-single-process` | actor-session binding 갱신이 실패하면 helper도 실패하고, local binding table의 같은 token entry도 제거된다 |
| stale session binding token guard | `integration-single-process` | 이전 stream에서 늦게 도착한 unbind나 stale bound session 메시지가 새 binding을 지우거나 사용하지 못한다 |
| location store 기반 SpotRef resolver 등록 | `unit` | location store 등록 시 기본 `ZLinkSpotRefResolver` 와 actor spot ref resolver 를 등록한다 |
| actor-bound session route 등록 | `integration-single-process` | actor-session route 는 session bind 시 actor runtime state 에 저장된다 |
| stale session unbind guard | `integration-single-process` | 이전 binding token 으로 도착한 disconnect 가 새 actor-session binding 을 지우지 않는다 |
| sample-only store 없이 framework/session 흐름 사용 | `unit` | TicTacToe.Ts 와 Bingo.Ts 샘플이 sample-only actor-session store 없이 framework/session 흐름을 사용한다 |
| stale bound session send | `integration-single-process` | 이미 닫힌 stream이나 stale binding으로 향하는 one-way push가 route receive loop와 host shutdown을 실패시키지 않는다 |
| bound session gateway relay | `integration-single-process` | Play 서버에서 Session 서버로 가는 bound session send가 core session relay binding 을 통해 client STREAM에 단일 stream packet으로 도착한다 |
| bound session disconnect local actor | `integration-single-process` | local actor 가 actor id 없이 `ZLinkBoundSession.disconnect(...)` 를 호출하면 binding 이 정리되고 session disconnect callback 은 다시 호출되지 않는다 |
| bound session disconnect remote actor | `integration-single-process` | remote actor 가 actor id 없이 `ZLinkBoundSession.disconnect(...)` 를 호출해도 session host 에서 같은 close 의미가 유지된다 |
| session context close | `integration-single-process` | `ZLinkSessionContext.close(...)`가 현재 stream client 연결을 서버 쪽에서 끊고, 이어서 disconnect callback으로 연결된다 |
| actor join 직후 packet dispatch | `integration-single-process` | join이 끝난 뒤 들어온 packet이 새 `Spot` 실행 문맥에서 실행된다 |
| actor spot 이동 직후 packet dispatch | `integration-single-process` | 이전 `Spot` 문맥으로 stale dispatch가 발생하지 않는다 |
| spot context channel request 경로 | `integration-single-process` | `spot.context.outbound.requestToChannel(channelName, request)`가 현재 MeshNode의 논리 channel을 사용한다 |
| spot context routed send/request 표면 | `contract`, `integration-single-process` | `ZLinkSpotOutbound`의 Spot·channel send/request와 publish가 현재 MeshNode의 route transport를 사용한다 |
| actor bound session send API | `integration-single-process` | actor는 `context.boundSession.send(...)`로 client stream에 push하고, `ZLinkStream`을 직접 노출받지 않는다 |
| actor request handler reply | `unit` | actor request packet은 actor request handler 반환값으로만 reply되고 send handler로 fallback dispatch되지 않는다. send/request 밖 stream kind도 actor packet으로 처리하지 않는다 |
| Spot actor request handler reply | `unit` | Entry Spot/user Spot actor request packet은 request handler 반환값으로만 reply되고 send handler로 fallback dispatch되지 않는다. send/request 밖 stream kind도 actor packet으로 처리하지 않는다 |
| local actor request relay reply | `integration-single-process` | local session actor relay도 actor request handler 반환값으로 stream response를 작성한다 |
| actor context reply를 public surface로 노출하지 않음 | `unit` | actor context reply와 actor stream client 계약이 public surface에 다시 노출되지 않는다 |

## 6. Stream Regression 항목

> dotnet `ContractTests/Streams`, `E2ETests/Stream` 미러. header 기반 단일
> `onDispatch` session 등록, lifecycle callback, handler invoker 표면을 검증한다.

| 항목 | 계층 | 통과 기준 |
|------|------|-----------|
| 같은 node에 session 중복 등록 | `unit` | startup validation 예외 |
| header session node | `integration-single-process` | `onDispatch(...)` 호출 확인 |
| `onConnected(...)` | `integration-multi-process` | `ConnectionReady` 이후 1회 호출 |
| `onError(...)` 범위 | `integration-multi-process` | transport error만 session callback으로 전달된다 |
| peer metadata 표면 | `integration-single-process` | `sessionId`, `routingId`, `localAddr`, `remoteAddr` 값 확인 |
| session callback task dispatch | `integration-single-process` | transport callback에서 user callback을 직접 호출하지 않고, managed task(microtask 큐) 경로로 호출한다 |
| session callback 직렬성 | `integration-single-process` | stream socket이 보존한 같은 session frame 순서대로 lifecycle/packet callback이 직렬 실행되며, 서로 병렬로 겹치지 않는다 |
| session callback 직접 호출 우회 방지 | `unit` | runtime 내부 transport 진입점은 enqueue API만 사용한다 |
| handler `Promise<T>` 결과 await | `unit` | handler invoker가 generic `Promise<T>`를 실제 결과 값으로 변환하고, 값 타입 변환 오류를 내지 않는다 |
| abstract wire payload validation | `unit` | converter 없는 abstract/interface payload가 node 경계 DTO에 포함되면 등록 시점 또는 첫 submit 직전에 configuration 오류로 실패한다 |

## 7. Location / Monitoring Regression 항목

공통 location runtime과 monitoring 계약을 Node 공개 표면으로 검증한다.

| 항목 | 계층 | 통과 기준 |
|------|------|-----------|
| monitoring source 이름 불일치 | `unit` | startup validation 예외 |
| location runtime polling diff | `integration-multi-process` | topology, status, service summary event가 발생한다 |
| spot polling diff | `integration-multi-process` | status, peers, subjects event가 발생한다 |

## 8. Release Gate

릴리스로 보내려면 다음 여섯 가지를 모두 만족해야 한다. 로컬에서는
`npm run verify:release` 가 아래 필수 gate 를 한 번에 실행한다. CI 는 같은 기준을
runtime/ABI matrix job 과 cross-language job 으로 나누어 실행한다.

1. `unit`, `integration-single-process`, `integration-multi-process` 전부 통과
2. `npm run verify:runtime-matrix` 로 `node20`, `node22` 양쪽 모두 통과
3. 위 여섯 플랫폼 ABI 전체에서 CI gate 통과
4. happy-path 샘플과 대표 failure-path가 각각 한 번 이상 커버되어 있음
5. 책임 spec에 정의한 비허용 조합이 모두 테스트로 고정되어 있음
6. `npm run verify:cross-language` 로 cross-language smoke 필수 경로가 통과되어
   Node 구현이 dotnet/C++/Java 와 같은 wire 계약을 지킨다는 것을 확인함

즉 샘플이 한 번 실행되는 것만으로는 충분하지 않다. startup validation 과
runtime failure 의미까지 테스트로 같이 고정되어 있어야 한다.

또한 native backend 가 이미 해당 플랫폼을 지원하더라도, framework 는 그 위에
registration, lifecycle, DI, monitoring 계층을 더 쌓는다. 그래서 플랫폼 gate 는
backend gate 와 별도로 유지한다.

## 8.1 Sample / Guide / Cross-Language Release 항목

> Phase 9 의 사용성·샘플 축은
> [정본 샘플](../README.ko.md)이 소유한다.
> 아래 항목은 release gate 에서 반드시 실행한다.

| 항목 | 계층 | 통과 기준 |
|------|------|-----------|
| `npm run verify:release` | `integration-multi-process` | ABI 선언, P0 회귀, sample smoke, Node runtime matrix, cross-language smoke 를 순서대로 실행한다 |
| `npm run verify:samples` | `integration-multi-process` | 유지하는 TypeScript sample 6종이 모두 self-check 통과 |
| `npm run verify:runtime-matrix` | `integration-multi-process` | 현재 runner 가 Node 20 과 Node 22 에서 build, typecheck, 전체 contract test 를 모두 통과시킨다 |
| `npm run verify:abi-matrix` | `unit` | `framework-node` CI workflow, release 문서, package script 가 `win-x64`, `win-arm64`, `linux-x64`, `linux-arm64`, `darwin-x64`, `darwin-arm64` 와 Node 20/22 gate 를 같은 목록으로 유지한다 |
| `npm run verify:cross-language` | `integration-multi-process` | Node 와 dotnet TestHost 가 channel/stream 필수 경로 여섯 가지를 같은 프로토콜 의미로 통과시킨다 |
| guide chapter map | `unit` | Node guide 12개 장이 dotnet guide 주요 장과 1:1로 매핑된다 |
| sample public API import guard | `unit` | sample 이 framework/connector public API만 import하고 binding internal/native 경로를 직접 쓰지 않는다 |
| sample readiness guard | `unit` | sample 이 sleep-only readiness masking을 사용하지 않고 observable readiness를 기다린다 |
| Node client -> dotnet channel server request/reply | `integration-multi-process` | dotnet request handler가 같은 payload 의미로 reply한다 |
| Node client -> dotnet channel server one-way send | `integration-multi-process` | dotnet send handler가 같은 packet 의미로 처리한다 |
| Node publisher -> dotnet fanout subscriber publish | `integration-multi-process` | dotnet publish handler가 같은 topic/payload 의미로 처리한다 |
| dotnet client -> Node channel server | `integration-multi-process` | Node handler가 dotnet client 요청에 같은 payload 의미로 reply한다 |
| Browser TypeScript connector -> dotnet stream server | `integration-multi-process` | Chromium에서 header session request/reply와 notification dispatch가 동작한다 |
| dotnet connector -> Node stream server | `integration-multi-process` | dotnet connector가 Node `onDispatch`와 `reply` 경로를 통과한다 |

## 8.2 Spot 비동기 직렬 실행 regression

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `entry-spot-serial-dispatch.test.js` 직렬 실행 항목 | Promise handler가 완료될 때까지 같은 actor 또는 Spot 실행 경계의 다음 작업이 시작되지 않는다. |
| `contract-surface.test.js` call declaration 항목 | actor, bound session과 route call object가 제거된 `yield(...)`를 public contract로 제공하지 않는다. |
| `npm run build --prefix framework/languages/node/samples/Bingo.Ts` | Bingo.Ts가 공개 `yield(...)` 없이 `submit(...)`과 Promise 완료 계약으로 compile된다. |

## 9. 문서별 회귀 테스트 단락

이 디렉토리(및 `spec/`)의 각 구현 기준 문서는, 자기 항목이 어떤 테스트로
고정되어 있는지 짧은 `회귀 테스트` 단락을 갖고 있어야 한다. 중앙 matrix 만
갱신해서는 곤란하다. 세부 문서의 독자가 어떤 테스트를 봐야 하는지 놓치기 쉽기
때문이다.

dotnet 의 문서 회귀 테스트처럼, Node 에서도 구현 기준 문서가 자기 회귀 테스트
단락을 유지하는지 확인한다.

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `documentation-regression.test.js › node guide exposes the 12 required guide chapters` | 아래 guide 문서가 모두 `회귀 테스트` 단락을 가진다. |
| `documentation-regression.test.js › node documentation relative markdown links resolve` | 이 matrix를 포함한 Node 문서의 상대 링크가 모두 유효하다. |

> dotnet 의 narrative guide 와 case-study 문서가 strict 집합에서 제외되는 것과
> 동일하게, node 의 사용자 가이드(usability) 계층은 strict 집합 대상이 아니다.
> 현재 node 묶음은 구현 기준 문서(`spec/`, `internals/`, root plan, sample plan)만
> strict 집합으로 둔다.

대상은 현재 실제로 존재하는 Node 공개 계약과 구현 기준 문서다.

- `framework/common/spec/server/languages/node/README.ko.md`
- `framework/common/spec/server/languages/node/interfaces/README.ko.md`와 그 범주별 interface 문서
- `framework/node/README.ko.md`
- `framework/node/internals/regression-test-matrix.ko.md`
- `../../common/internals/README.ko.md`
- `framework/node/internals/backend-dependency-policy.ko.md`

[^public-contract]: public contract 는 외부 사용자에게 공개되어 변경 시 호환성을 책임져야 하는 API 표면을 뜻한다.
[^regression]: regression(회귀) 은 이전 버전에서 잘 동작하던 기능이 새 변경 때문에 다시 깨지는 현상을 가리킨다. regression test 는 그런 일을 막기 위해 항상 돌리는 테스트 묶음이다.
[^topology]: topology 는 어떤 노드(channel, spot, registry 등)가 어디에 있는지, 그리고 서로 어떻게 연결되어 있는지를 나타내는 구성 정보다.
[^abi]: ABI(Application Binary Interface) 조합은 Node.js native addon 이 동작하는 OS·CPU 아키텍처 조합을 가리킨다. 예: `linux-x64`, `darwin-arm64`. `@zlink-systems/zlink` prebuilt artifact 가 이 조합으로 배포된다.
[^ci-gate]: CI gate 는 새 변경을 머지하거나 배포하기 전에 통과해야 하는 자동 검증 단계(빌드, 테스트 등)의 묶음을 가리킨다.
[^di]: DI(Dependency Injection) 는 객체가 필요한 의존성을 직접 만들지 않고 외부 컨테이너에서 주입받도록 하는 패턴이다. NestJS 에서는 module + provider + token 기반 컨테이너가 표준이다.
[^backpressure]: backpressure는 송신 속도가 수신 측의 처리 용량을 넘지 않도록 흐름을 조절하는 메커니즘이다.
[^hwm]: HWM(High Water Mark) 은 송신 큐가 보관할 수 있는 최대 byte 를 가리키며, 이 한계에 도달하면 backpressure 가 발동한다.
[^wire-multipart]: wire multipart 는 한 논리 메시지를 header, payload 등 여러 message part 로 나누어 전송하는 방식이다. 한쪽만 떼어 살펴봐도 라우팅이 가능해진다.
[^entry-spot]: Entry Spot은 MeshNode에 들어온 actor를 가장 먼저 처리하는 진입용 Spot이다. 이후 user Spot으로 이동하기 전 단계 역할을 한다.
[^spot]: `SPOT`은 동적으로 생성·소멸되는 논리적 노드(예: room, stage 등) 단위로 메시지를 라우팅하는 추상이다. MeshNode는 하나 이상의 Spot 인스턴스를 호스팅한다.
[^binding-token]: session binding token 은 actor 와 stream session 의 연결 상태를 식별하는 토큰으로, 재연결 시 어느 binding 이 최신인지 구분하는 데 쓰인다.

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../README.ko.md) | [이전: Runtime Lifecycle](../../common/internals/README.ko.md) | [다음: Backend Dependency Policy](backend-dependency-policy.ko.md)
<!-- framework-adapter-nav:bottom:end -->

## 공개 계약 문서에서 이관한 회귀 항목

언어별 spec을 3문서(시스템 구조 · 인터페이스 · connector)로 압축하면서, 삭제한 기능별 계약
문서가 소유하던 회귀 항목을 이 절로 옮겼다. 계약의 의미는 공통 스펙이 소유한다.

### Channel

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `Channels.forRoot_Throws_WhenChannelNameIsDuplicated` | 같은 channel 이름을 중복 등록하면 startup validation 예외가 난다. |
| `Channels.forRoot_Throws_WhenClientHasNoPeerAcquisitionPath` | client 역할에 Discovery나 수동 연결이 없으면 시작 전에 실패한다. |
| `ClientServer.ManualClient_Request_And_Send_Work_Across_Hosts` | 수동 연결 client가 request와 send를 모두 처리한다. |
| `ClientServer.DiscoveryClient_Request_And_Send_Work_Across_Hosts` | Discovery 기반 client가 request와 send를 모두 처리한다. |
| `FiltersAndHttp.HttpHandler_Uses_SameContainer_ToResolve_ZLinkChannelClient` | HTTP controller가 같은 DI container에서 `ZLinkChannelClient`를 받아 호출한다. |
| `channel runtime drains backpressured requests from send-ready callback` | async submitter가 ready callback에서 pending item을 비우고 중복 전송하지 않는다. |

### SPOT

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `forRoot throws when spot factory class is duplicated across nodes` | 같은 Spot factory 클래스를 중복 등록하면 startup validation 예외가 난다. |
| `forRoot allows standalone local spot node` | location store 없이도 수동 peer 연결만 사용하는 MeshNode 구성을 시작할 수 있다. |
| `spotManager create/find/list/close work through framework runtime` | `create`, `find`, `list`, `close` 와 scope 정리가 일관된다. |
| `spot publish/timer and close stop callbacks work` | timer와 publish callback이 Spot lifecycle 문맥에서 실행되고, 종료 뒤에는 추가로 실행되지 않는다. |
| `spot timer provides tick metadata` | timer handler 가 callback 번호, 예정/시작 시각, 지연, skip metadata 를 받는다. |
| `spot timer skips late ticks when configured` | `SkipLateTicks` 정책은 늦은 tick 을 무제한 전달하지 않고 `skippedTicks` 로 드러낸다. |
| `spot timer catches up within configured limit` | `CatchUpBounded` 정책은 `maxCatchUpTicks` 상한 안에서만 연속 실행한다. |
| `spot timer delayNextTick waits after handler completion` | `DelayNextTick` 정책은 handler 완료 뒤 period 를 다시 기다린다. |
| `spot timer rejects unknown overrun policy` | 알 수 없는 overrun 정책 값은 설정 오류다. |
| `spot timer reports handler exception to monitoring` | handler 예외가 runtime monitoring 의 timer failure event 로 기록된다. |
| `spot timer stopOnUnhandledException stops timer` | `stopOnUnhandledException` 이 켜진 timer 는 첫 handler 예외 뒤 중단된다. |
| `spot timer cancel stops managed timer loop` | `cancel()` 뒤 managed timer loop 가 추가 callback 을 실행하지 않는다. |
| `outbound-only spot publisher client publishes to target channel` | 외부 publisher client 가 target SPOT channel 로 publish 한다. |
| `spot actor join/move/submit run through spot execution context` | actor join, 이동, packet dispatch 가 현재 spot 실행 문맥에서 실행된다. |
| `entry spot actor packets use actor mailboxes without entry-wide serial dispatch` | Entry Spot actor packet 은 대상 actor mailbox 를 사용하고, 서로 다른 actor 는 Entry Spot 전체 실행 줄 때문에 서로 기다리지 않는다. |
| `entrySpot packet handlers use entrySpot serialization` | Entry Spot 일반 packet handler 가 user Spot 처럼 Spot 단위 직렬 실행 줄을 사용한다. |
| `entrySpot timer waits for entrySpot callbacks` | Entry Spot timer callback 이 같은 Entry Spot의 다른 callback 과 동시에 실행되지 않는다. |
| `entrySpot timer does not reenter same timer` | Entry Spot timer 는 같은 timer callback 을 겹쳐 실행하지 않는다. |

### Actor

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| --- | --- |
| `actorFactoryNameIsDuplicated` | actor factory 이름(actorType)이 중복되면 startup validation에서 예외로 막는다. |
| `entrySpotAndUserSpotActorPacketRegistriesDispatch` | Entry Spot과 user Spot에 등록한 actor packet handler와 disconnected handler가 정상적으로 dispatch된다. |
| `entry spot callbacks from mixed setImmediate/queueMicrotask backend callbacks keep enqueue order without overlap` | Entry Spot callback 이 backend task 문맥과 무관하게 Entry Spot 실행 줄에서 순서대로 실행된다. |
| `entry spot does not start the next callback before the previous handler promise settles` | Entry Spot handler Promise 가 끝나기 전에는 같은 Entry Spot 의 다음 callback 이 시작되지 않는다. |
| `spotActorJoinMoveAndSubmitRunThroughSpotExecutionContext` | actor가 spot을 옮긴 뒤 stale spot 문맥으로 dispatch되지 않는다. |
| `sessionActorDispatchRelaysStreamRequestAndRoutesBySequence` | stream session에서 bound actor로 request가 전달되고, sequence별 reply 순서가 맞는다. |
| `localSessionActorDispatchRepliesFromRequestHandler` | local actor relay 도 request handler 반환값으로 stream response 를 작성한다. |
| `spotActorRegistryDoesNotResolveRequestToSendHandler` | Entry Spot/user Spot actor request packet 이 send handler 로 fallback dispatch 되지 않고, send/request 밖 stream kind 도 actor packet 으로 처리되지 않는다. |
| `publicSurfaceRemovesActorReplyAndStreamClientContracts` | actor context reply 와 actor stream client 계약이 public surface 에 다시 노출되지 않는다. |

### STREAM

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `nodesAndServices.throwsWhenStreamNodeRegistersMultipleSessions` | 같은 node에 session을 중복 등록하면 startup validation 예외가 발생한다. |
| `ZLinkModule.forRoot maps stream node options into runtime registration` | Node builder 표면에서도 같은 stream node 에 session 을 두 번 등록하면 startup validation 예외가 발생한다. |
| `protocol.streamSessionRuntimeOnlyExposesEnqueueCallbackEntrypoints` | transport 진입점은 public enqueue API만 노출한다. |
| `stream session node runtime does not invoke user callbacks inside transport callback` | transport callback 은 user `onDispatch(...)` 를 같은 호출 스택에서 직접 실행하지 않고 managed queue 로 넘긴다. |
| `headerStreamSession.receivesRepliesAndTracksLifecycle` | connected, dispatch, reply, metadata, disconnected/error callback이 기대한 순서대로 실행된다. |
| `headerStreamSession.canCloseCurrentClientStream` | session context가 현재 client stream을 서버 쪽에서 닫을 수 있다. |
| `stream session and bound session require packetName for structural payloads` | 구조적 payload 는 stream session send 와 bound session send 양쪽에서 명시 packet name 없이 전송되지 않는다. |

### Monitoring

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `socket monitoring source maps backend raw events into framework typed events` | socket backend event를 framework의 typed event로 변환한다. |
| `location runtime monitoring source publishes snapshot changes and suppresses unchanged polls` | location runtime snapshot이 바뀔 때만 상태·topology·service summary event를 전달한다. |
| `location monitoring event emitter publishes registered row and resolve-miss events` | location row 등록과 resolve 실패를 typed event로 전달한다. |
| `spot monitoring source publishes status peers and subjects snapshot changes` | Spot의 status·peer·subject snapshot 변경을 typed event로 전달한다. |
| `spot timer reports handler failure immediately through runtime publisher` | timer handler 예외를 runtime publisher를 통해 즉시 전달한다. |
| 공통 개념 | Node 타입 / 멤버 |
| 로그 모드 | `ZLinkMessageFlowLogMode` { `Off`, `ErrorsOnly`(기본), `KeyTransitions`, `Verbose` } |
| outcome | `ZLinkMessageFlowOutcome` { `succeeded`, `failed`, `backpressured`, `dropped`, `cancelled`, `shutdown` } |
| event | `ZLinkMessageFlowEvent`: `eventId`, `outcome`, `surface`, `messageKind`, `phase?`, `packetName?`, `meshName?`, `channelName?`, `topic?`, `correlationId?`, `sourceRid?`, `targetRid?`, `spotRid?`, `actorId?`, `messageSizeBytes?` |
| observer | `ZLinkMessageFlowObserver.onMessageFlow(flow): Promise<void> \| void` |
| 진단 옵션 | `ZLinkDiagnosticsOptions` { `messageFlow`, `sampleRate`, `includeMessageSizes`, `logFile?`, `label?` } |
| 런타임 토글 | host `ZLinkMessageFlowControl.setMessageFlowMode(mode)` / `messageFlowMode()` |
| 공통 개념 | Node.js |
| meter 이름(상수) | `ZLinkMeters.Framework` = `'zlink.framework'` |
| 계기 방출 | OpenTelemetry Metrics API `Meter` — `Counter`/`UpDownCounter`/`ObservableGauge`/`Histogram` |
| 앱 연결(공통 케이스) | 전역 OTel `MeterProvider`(SDK) 구성 — 별도 zlink 설정 없음 |
| 커스텀(선택) | `ZLinkModule.forRoot(zlinkFramework().options({ metrics: { meterProvider } }).build())`로 provider 주입 |
| 공통 개념 | Node.js |
| 생성 게이트 | 기존 `configureDispatch().messageFlow(...)` 설정을 그대로 사용한다. 별도 flow id 설정은 없다. |
| event 필드(추가) | `readonly flowId: string`, `readonly flowOrigin: ZLinkFlowOrigin` — 오류 이벤트에도 동일한 root 값 |
| 공통 개념 | Node.js |
| 자동 종료(기본) | framework가 `onApplicationShutdown()`에서 진행 중인 host 종료에 합류하거나 `Shutdown`을 시작한다 |
| `Shutdown` 순서 | 신규 application 수락 차단 → 이미 수락한 실행 차례와 request 완료 → 진행 중인 relocation·STREAM barrier 확인 → local object·ownership·peer resource 정리 → 필요하면 제한된 강제 종료 |
| `Retire` 순서 | all-or-none preflight → target reservation → admission seal → Actor·Instance Spot continuity relocation → STREAM barrier → host resource 정리 |
| Spot 재생성 경계 | public `create`·`getOrCreate`는 local-only다. Instance address cold activation과 명시적 `Retire` target materialization만 별도 계약으로 실행되며 stale handle은 숨은 remote create를 시작하지 않는다 |
| 명시 제어 | host singleton `ZLinkFrameworkRuntime`의 `retire(options?)`와 `shutdown(options?)`; 기본 deadline은 30,000ms이고 `AbortSignal`은 waiter만 끝낸다 |
| 종료 결과 | `ZLinkTerminationResult`가 effective intent, `Stopped|Blocked|ForceStopped` outcome과 닫힌 reason을 함께 제공한다 |
| readiness probe | framework가 NestJS Terminus `ZLinkDrainHealthIndicator(runtime, meshName)`를 제공해 특정 MeshName의 readiness를 확인한다. 또는 `ZLinkRouteMeshRuntime.isReady(meshName)`을 직접 조회한다 |
| Mesh drain 결과 | `ZLinkRouteMeshRuntime.drain(...)`과 `awaitDrained(...)`는 `ZLinkMeshDrainResult`를 반환한다. 강제 종료 reason은 `deadline_exceeded`, `drain_state_publish_failed`, `owner_cleanup_failed`, `teardown_failed` 네 값으로 제한한다 |
| 상태 관측 | host 종료는 `ZLinkFrameworkRuntime.observe(...)`의 `zlink.runtime.host.termination_changed`로, Mesh projection은 `ZLinkRouteMeshRuntime.observe(...)`로 확인한다 |

### Session actor dispatch

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `RemoteSessionRelayTests.SessionActorDispatch_Relays_Stream_Request_And_Routes_Request_To_Bound_Actor_By_Sequence` | session callback에서 actor request를 relay하고, request sequence를 통해 reply를 되돌린다. |
| `ActorDisconnectNotifyTests.ClientClose_Cleans_Session_Without_Actor_Disconnect_Callback` | client stream close 는 session binding cleanup 만 수행하고 Actor disconnect callback 을 호출하지 않는다. |
| `ActorBindingTests.BindActorAsync_DoesNot_Create_LocalActor` | logical actor binding 은 session attach 중 local actor 를 새로 만들지 않는다. |
| `ActorBindingTests.SessionActorBind_WithoutRoute_Is_LocalOnly` | route 없는 bind overload 는 local actor 에만 붙고 remote fallback 을 수행하지 않는다. |
| `RemoteProxyDisconnectTests.BoundSessionDisconnect_FromRemoteActor_Closes_Client_Without_Session_Disconnect_Callback` | remote actor 가 `boundSession.disconnect(...)` 를 호출해도 session host 에서 같은 close 의미가 유지된다. |
| `entry spot callbacks from mixed setImmediate/queueMicrotask backend callbacks keep enqueue order without overlap` | backend callback 이 서로 다른 task 문맥에서 도착해도 Entry Spot 실행 줄에서 등록 순서대로 겹치지 않고 실행된다. |
| `entry spot does not start the next callback before the previous handler promise settles` | handler Promise 가 끝나기 전에는 같은 Entry Spot 의 다음 callback 이 시작되지 않는다. |
| `entry spot actor packets use actor mailboxes without entry-wide serial dispatch` | Entry Spot actor packet 은 대상 actor mailbox 를 사용하고, 서로 다른 actor 는 Entry Spot 전체 실행 줄 때문에 서로 기다리지 않는다. |
| `LocalActorMailboxExecutionTests.LocalActorPackets_Are_Serialized_Per_Actor_And_Parallel_Across_Actors` | user Spot에 들어가지 않은 actor packet도 actor별 순서를 지키되 서로 다른 actor 사이에서는 병렬로 실행될 수 있다. |
| `ActorRegistryExecutionTests.ActorDispatch_Rechecks_CurrentLocation_After_Waiting_For_ActorMailbox` | 같은 actor의 앞 packet이 join을 마치고 나면, 대기 중이던 다음 packet이 새 user Spot 위치로 dispatch된다. |
| `ActorLifecycleTests.SpotActorJoin_Move_And_Submit_Run_Through_SpotExecutionContext` | actor join 이후의 dispatch가 현재 spot 실행 문맥에서 실행된다. |
| `ActorSessionStateTests.ActorSessionState_Filters_StaleDisconnect_And_Only_Disconnects_CurrentStream` | 이전 stream의 늦은 disconnect가 현재 actor-session 연결을 끊지 않는다. |
| `HeaderStreamSessionTests.HeaderStreamSession_Can_Close_Current_Client_Stream` | session context가 현재 client stream을 닫고 disconnect callback으로 자연스럽게 이어진다. |
| `SerialExecutorTests.StreamSessionSerialExecutor_Continues_After_Work_Exception` | session queue의 fire-and-forget work 예외가 error sink에 기록되고, 다음 work 실행을 막지 않는다. |
| `SerialExecutorTests.SpotSerialExecutor_Continues_After_Queued_Work_Exception` | Spot queue의 fire-and-forget work 예외가 error sink에 기록되고, 다음 work 실행을 막지 않는다. |
| `runtime task runner observes detached task exceptions without unhandled rejection` | Node runtime task runner 가 detached task 예외를 관찰하고 unhandled rejection 을 만들지 않는다. |
| `framework runtime state aborts listener tasks before disposing backend context` | runtime state shutdown 이 listener task 에 stop signal 을 먼저 전달하고 backend context 를 마지막에 정리한다. |
| `SerialExecutorTests.SpotSerialExecutor_ExecuteAsync_Propagates_Work_Exception` | Spot queue에서 완료를 기다리는 실행 경로는 handler 예외를 호출자에게 그대로 돌려준다. |
| `SerialExecutorTests.SerialExecutionQueue_RunAsync_Propagates_Work_Exception` | 공통 serial queue의 `run(...)`가 work 예외를 error sink에 기록하면서 호출자에게도 전파한다. |
| `SerialExecutorTests.SerialExecutionQueue_Wait_Cancellation_Does_Not_Remove_Queued_Work` | 공통 serial queue에서 completion wait가 취소되더라도 이미 queue에 들어간 work item은 제거되지 않는다. |
| `SerialExecutorTests.ActorDispatchCancellation_Does_Not_Stop_Current_Or_Later_Dispatch` | actor dispatch 대기를 취소해도 현재 실행 중인 dispatch나 이후 dispatch가 중단되지 않는다. |
| `RegressionTests.NodeSessionActorDispatch_Documents_ExecutionSerialization_Core_Code` | 실행 직렬화 핵심 코드 섹션이 queue, runtime task, error sink, cancellation 의미를 계속 설명한다. |
| `RegressionTests.NodeRegressionMatrix_Includes_ExecutionSerialization_Guards` | 중앙 regression matrix가 실행 직렬화 관련 회귀 항목을 유지한다. |

### MeshNode와 Spot

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `EntryRoutingTests.EntrySpotRoutingId_IsApplied_ToNativeEntrySpot` | `entrySpot.routingId` 로 지정한 routing id 가 native Entry Spot facade 에 적용되고 Entry Spot activation 의 `spotRid` 로 노출된다. |
| `LocationRuntimeTests.SpotRefResolver_Resolves_Created_Spot_By_Rid_And_Removes_Route` | Spot RID route는 Spot rid만 찾는 색인으로 쓰고, resolver가 유효한 spot location row의 owner node rid와 `SpotKind.User`를 보존한다. |
| `ManagerTests.SpotManager_Create_List_Close_And_Publish_Work_Through_FrameworkRuntime` | `create`, `find`, `list`, `close` 와 scope 정리가 일관되게 동작한다. |

### Stage wrapper

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `E2E:SM-B7` | actor join 뒤 stage 역할의 Spot에서 packet이 lifecycle 순서에 맞게 처리된다. |
| `E2E:SM-E3` | stage tick으로 쓰는 timer가 Spot 종료 뒤 추가 callback을 만들지 않는다. |
| `E2E:SM-A5` | application stage wrapper가 Spot request, timer와 lifecycle을 public API로 실행한다. |

### Bootstrap/Overview

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `backend-contract.test.js` | backend adapter factory가 channel, Spot, stream, monitoring adapter를 모두 제공한다. |
| `backend-public-api-only.test.js` | framework runtime 이 binding internal/native 경로를 직접 import 하지 않는다. |
| `nestjs-module.test.js` | `ZLinkModule.forRoot/forRootFactory`, provider token 노출, startup validation, 실제 NestJS application context 주입, lifecycle 연결이 동작한다. |
| `documentation-regression.test.js › node implementation reference docs declare regression coverage sections` | 이 overview 가 자기 회귀 테스트 단락을 유지한다. |
