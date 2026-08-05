<!-- framework-adapter-nav:start -->
[Document List](../README.en.md) | [Previous: Runtime Lifecycle](../../common/internals/README.en.md) | [Next: Backend Dependency Policy](backend-dependency-policy.en.md)
<!-- framework-adapter-nav:end -->

[Node.js Bundle](../README.en.md) | [Runtime Lifecycle](../../common/internals/README.en.md) | [Backend Policy](backend-dependency-policy.en.md) | [Common E2E](../../common/e2e/README.en.md)

# ZLink Framework Node.js Regression Test Matrix

> The basis for regression items is the implementation in `framework/languages/node` and the Node
> tests (`node:test`-based `*.test.js`, `test(...)`). The dotnet regression matrix is treated only as
> an optional parity-comparison reference.

## 1. Purpose

This document lays out, test item by test item, what counts as a regression when the implementation
changes. The responsible spec owns the public behavior; this document owns only verification and the
release gate.

The dotnet test project's contract/unit/E2E split is used as a reference when comparing. Node uses
its own directory and test-name conventions based on `node:test`, so file structure or test names are
not copied verbatim.

| dotnet Project | Node Test Package | Layer Mapping |
|------|------|------|
| `Zlink.Framework.ContractTests` | `@zlink-systems/framework` contract tests (`test/contract/**/*.test.js`) | `unit` contract surface + partial `contract` |
| `Zlink.Framework.UnitTests` | unit tests (`test/unit/**/*.test.js`) | `unit` |
| `Zlink.Framework.E2ETests` | e2e tests (`test/e2e/**/*.test.js`) | `integration-single-process` + `integration-multi-process` |

> Whether the common public contract requires the same behavior is compared, but a Registry runtime
> or test bundle that doesn't exist in Node is not carried over as-is from the comparison language.

## 2. CI Layers

Regression tests are split into the following three layers.

| Layer | Purpose | Example |
|------|------|------|
| `unit` | registration validation, dispatch lookup, option parsing | duplicate registration, module options validation |
| `integration-single-process` | confirms runtime combinations work normally within the same host (Node process) | channel request/send, location runtime, monitoring attach |
| `integration-multi-process` | confirms actual topology[^topology] and reconnect behavior | remote registry query, discovery change, spot peer change |

## 3. Minimum CI Matrix

| Item | Standard |
|------|------|
| Node.js runtime | `node20` (LTS), `node22` (current) |
| Platform ABI[^abi] | `win-x64`, `win-arm64`, `linux-x64`, `linux-arm64`, `darwin-x64`, `darwin-arm64` |
| test mode | development (source `*.ts`), production (bundled/`dist`) |

Since the minimum supported runtime is `node20`, regression tests must also run both runtime
versions above together.

- The repository's default build is currently a single `node20` runtime.
- `node22` is handled as an extra compile/run in the multi-runtime build for regression-matrix
  reporting.

Meanwhile, the repository's `@zlink-systems/zlink` (Node binding) prebuilt native artifact
combination and the native artifact combination produced by the CI workflow are both based on the six
platform ABIs above. The framework CI gate[^ci-gate] also treats this as the default scope by
default.

In other words, the Node framework regression tests don't run just one representative OS and call it
done. Under the current plan, the platforms that must pass are:

- Windows x64
- Windows ARM64
- Linux x64
- Linux ARM64
- macOS x64
- macOS ARM64

## 3.1 Node Binding Parity Regression Items

> The framework sits only on top of the `@zlink-systems/zlink` public API. Just as dotnet's
> `Runtime/Backend/DotNet/` uses `bindings/dotnet`'s public surface, the Node backend adapter also
> does not directly bypass the binding's internal/native path.

| Item | Layer | Pass Criteria |
|------|------|-----------|
| binding public API parity | `unit` | Channel, Spot, stream, monitoring, session relay, and bound session all work using only the binding public API |
| framework public-api-only import guard | `unit` | The framework runtime/adapter package doesn't import binding internal paths, native addon symbols, or generated private helpers |
| session relay public API smoke | `integration-single-process` | Stream session relay works using only the binding public API (no separate attach) |
| bound session public API smoke | `integration-single-process` | Bound session send/disconnect work using only the binding public API |
| registry query public API smoke | `integration-single-process` | The registry query client wrapper calls only the binding public API |
| socket monitor public API smoke | `integration-single-process` | The socket monitoring source calls only the binding public API |
| native artifact freshness guard | `unit` | The framework smoke fails if the native addon build output is older than the source |

## 4. Channel Regression Items

> Mirrors dotnet's `ContractTests/Channels`, `ContractTests/Handlers`, `ContractTests/Configuration`,
> `E2ETests/Channels`. Verifies Node's `addRouteMesh(meshName)` / `channelName(channelName)`
> registration and the `ZLinkChannelClient` / `ZLinkRouteClient` / `ZLinkFanoutClient` surface.

| Item | Layer | Pass Criteria |
|------|------|-----------|
| duplicate MeshName registration (`addRouteMesh`) | `unit` | startup validation exception |
| empty MeshName or empty ROUTER endpoint | `unit` | startup validation exception |
| `addRouteMesh(...).listen(...)` + `channelName(...)` | `integration-single-process` | request/send succeeds on the specified MeshNode and logical channel |
| `peerConnections().connect(...)` | `integration-single-process` | request/send succeeds using a manual peer connection |
| `.addFanoutChannel(...)` + `.enableSubscriber(endpoint)` | `integration-single-process` | manual-based subscribe succeeds |
| no bind endpoint on a publisher role | `unit` | startup validation exception |
| publisher-only channel | `integration-single-process` | publish submit succeeds |
| subscriber discovery attach | `integration-multi-process` | remote publish is received |
| handler group mapping | `unit` | a handler decorator registration alone does not become a global dispatch target — only handlers of a group mapped by the channel's `handlerGroups: ['...']` are dispatched on that channel |
| server channel with no handler exposure | `unit` | even if a handler is scanned, it isn't automatically exposed to the application handler without `addHandlerGroup(...)` or `add*Handler(...)` |
| server channel with no handler exposure validation | `unit` | a server channel with no handler exposure doesn't open the application handler |
| fanout subscriber handler exposure | `unit` | only a publish handler registered via `addPublishHandler(...)` or a handler group is exposed as a subscriber dispatch target |
| typed handler registration | `unit` | a handler registered directly through the channel's `add*Handler(...)` is exposed to that channel even without group mapping |
| channel type handler compatibility | `unit` | RouteMesh allows send/request, and a fanout subscriber allows only a publish handler; registration mismatched with the channel's role is rejected |
| incompatible handler group mapping | `unit` | if a handler mismatched with the channel type is mixed into a group, it fails with a startup validation error instead of excluding only that part |
| route mesh handler group mapping | `integration-single-process` | a route mesh channel's `addHandlerGroup(...)` exposes the route send/request handler group as an actual routed dispatch target |
| route mesh packet dispatcher | `integration-single-process` | a routed send/request packet arriving through the route mesh `ROUTER` is dispatched to a handler, returns the request reply/error, and an empty probe frame is not passed to the application handler |
| DI route channel inbound handler dispatch | `integration-single-process` | `ZLinkModule.forRoot(...)`'s route channel `sendHandlers`/`requestHandlers` are connected to the host-owned `ROUTER` receive loop after the runtime host starts, handling routed send/request |
| DI route client host transport | `integration-single-process` | the `ZLinkRouteClient` exposed by `ZLinkModule.forRoot(...)` performs routed send/request/reply to a target node RID through host-owned ROUTER transport after the framework runtime host starts |
| Spot route-transport-only membership | `integration-single-process` | registering only a Spot factory and a logical channel on a MeshNode makes it work as Spot route transport, without exposing an unregistered application handler |
| duplicate handler on the same channel server | `unit` | startup validation exception if two or more handlers share the same `kind + packetName` |
| the same packet handler on different channel servers | `integration-single-process` | mapping the same `kind + packetName` to different channels still dispatches each channel independently |
| mapping the same group to multiple channels | `integration-single-process` | exposing the same `zlinkRequestHandler('api', ...)` group to two channels via `handlerGroups` still keeps the dispatch namespace independent per channel |
| `addHandlerGroup` pointing to a nonexistent group | `unit` | a startup validation error if the mapped group has no handler at all |
| event handler group mapping | `unit` | publish handler group mapping is added after the subscriber handler registration surface exists |
| using `ZLinkChannelClient` from an HTTP (REST controller) handler | `integration-single-process` | works normally in the same NestJS DI[^di] container as a route handler |
| DI channel client host transport | `integration-single-process` | the `ZLinkChannelClient` exposed by `ZLinkModule.forRoot(...)` performs a manual channel request/reply through host-owned DEALER transport after the framework runtime host starts |
| using `ZLinkChannelClient` from a channel handler | `integration-single-process` | an ordinary request handler requests a different channel with the same DI container's `ZLinkChannelClient` and receives a reply |
| fanout publish from a channel handler | `integration-single-process` | an ordinary request handler publishes a fanout event with the same DI container's `ZLinkFanoutClient`, and a subscriber handler receives it |
| send one-way submit backpressure[^backpressure] | `integration-single-process` | `submit()` attempts admission immediately once, and if capacity is insufficient, returns the result of waiting up to the send timeout |
| publish one-way submit backpressure | `integration-single-process` | uses the same admission contract as send and does not wait for remote receipt completion |
| separating request submit/reply timeout | `integration-single-process` | a request packet's submit delay is judged by the submitter timeout policy, and the reply wait by `timeout(...)` |
| pending request cleanup | `unit` | the request sequence is removed from the pending map on submit failure, timeout, cancellation (`AbortSignal`), or runtime stop |
| ready callback batch drain | `integration-single-process` | once the socket is ready, pending sends/publishes are processed as a batch, without duplicate-sending the same frame |
| channel wire multipart[^wire-multipart] | `integration-single-process` | server-to-server channel send/request/reply sends `header` and `payload` as separate message parts, and handler dispatch selects the packet by looking only at the header part |
| publish wire multipart | `integration-single-process` | `PUB/SUB` publish also keeps the framework header and payload as separate parts, and only the typed payload is delivered to the subscriber handler |

`submit()` for one-way send and publish asynchronously returns a bounded admission result, but does
not wait for remote receipt or handler execution completion.

## 4.1 Dispatch Error Observer Regression Items

| ID | Layer | Test Location | Pass Criteria |
|----|------|-------------|-----------|
| DERR-001, DERR-007, DERR-011, DERR-014 | `unit` | `test/contract/channel-client.test.js` | A missing channel request handler ends in an error reply plus observer event, a missing channel send handler ends in a drop plus observer event, and an observer exception doesn't break the original dispatch result |
| DERR-002, DERR-008 | `unit`, `integration-single-process` | `test/contract/channel-client.test.js` | A missing route request handler ends in an error reply; a missing route send handler ends in a drop plus observer event |
| DERR-003, DERR-004, DERR-009, DERR-010, DERR-016 | `unit`, `integration-single-process` | `test/contract/spot-manager.test.js`, `test/contract/actor-manager.test.js` | A SPOT route, subscription, or actor dispatch failure ends in an error reply or caller-visible rejection for a request, or a drop plus observer event for one-way |
| DERR-005, DERR-006, DERR-013, DERR-015 | `unit`, `integration-single-process` | `test/contract/channel-client.test.js`, `test/contract/spot-manager.test.js` | Decode failures and handler exceptions end in an error reply or an observable drop, with the default log and counter still recorded even without an observer registered |

## 4.2 DI Capability Regression Items

> Mirrors dotnet's `ContractTests/Configuration/ConnectionAndConfigContracts`,
> `UnitTests/Configuration/Registration`. Verifies NestJS provider token exposure rules and
> per-role provider exposure conditions.

| Item | Layer | Pass Criteria |
|------|------|-----------|
| actor factory without MeshNode | `unit` | startup validation exception if only an actor factory is registered |
| actor manager without MeshNode | `unit` | the `ZLinkActorManager` provider token is absent from DI in a configuration with no MeshNode |
| actor manager with MeshNode only | `unit` | `ZLinkActorManager` is absent from DI if there's a MeshNode but no actor factory |
| actor manager with MeshNode and actor factory | `unit` | `ZLinkActorManager` is registered in DI when both the MeshNode and actor factory exist |
| Spot service without MeshNode | `unit` | `ZLinkSpotManager` is absent from DI in a configuration with no MeshNode |
| Spot service with MeshNode | `unit` | the Spot service is registered in DI when a MeshNode exists |
| Spot publisher without a publisher role | `unit` | even with a MeshNode, the Spot publisher service is absent from DI without a publisher role |
| Spot publisher with a publisher role | `unit` | `ZLinkSpotPublisherClient` is registered in DI when the Spot publisher role exists |
| bound session factory registration | `unit` | `ZLinkBoundSessionFactory` is registered together with the framework runtime |
| SpotRef resolver without MeshNode | `unit` | a server with a registered location store can register `ZLinkSpotRefResolver` without a MeshNode |
| Spot outbound with resolver only | `unit` | `ZLinkSpotOutbound` is absent from DI if only the SpotRef resolver exists and there's no MeshNode |
| route channel missing at call time | `unit` | `ZLinkConfigurationException` when calling `ZLinkRouteClient` with no route channel |
| channel client missing at call time | `unit` | `ZLinkConfigurationException` when calling `ZLinkChannelClient` with no channel client role |

## 5. Spot Regression Items

> Mirrors dotnet's `ContractTests/Spots`, `ContractTests/Actors`, `E2ETests/Spot`. Verifies the
> `ZLinkSpotManager`, `ZLinkActorManager`, Entry Spot, and bound session surface.

| Item | Layer | Pass Criteria |
|------|------|-----------|
| duplicate Spot factory type | `unit` | startup validation exception |
| duplicate `addEntrySpot(EntrySpotClass)` | `unit` | startup validation exception on duplicate Entry Spot[^entry-spot] registry registration within the same MeshNode |
| `addRouteMesh(meshName)` + `addEntrySpot(...)` | `integration-single-process` | the MeshNode builder finishes Entry Spot and Spot factory registration in one go |
| registering a local MeshNode with no location store | `integration-single-process` | starts a MeshNode runtime using only manual peer connections |
| `create(spotType)` | `integration-single-process` | `spotId` and `Created` status stay consistent |
| `create(spotType)` empty create payload | `integration-single-process` | creation with no payload also calls `ZLinkSpot.onCreate(...)` exactly once with an empty `ZLinkMessage` |
| `create(spotType, request)` payload | `integration-single-process` | the create request `ZLinkMessage` is delivered exactly once to `ZLinkSpot.onCreate(...)` |
| `getOrCreate(spotType, spotRid, request)` existing | `integration-single-process` | if the same `spotId` is already ready, it's `Existing`, and the new `request` is not delivered to `onCreate(...)` |
| `getOrCreate(...)` concurrent create payload | `integration-single-process` | under concurrent creates of the same `spotId`, only the first creation request's `ZLinkMessage` is delivered to `onCreate(...)`, and the callback runs exactly once |
| `getOrCreate(spotType, spotRid)` same type | `integration-single-process` | re-acquiring the same `spotId` with the same Spot type returns the existing spot without calling `onCreate(...)` again |
| spot create lifecycle failure | `integration-single-process` | a failure in `onCreate(...)` or `onInitialize(...)` propagates as `SpotCreateFailed`, and the failed entry is removed so the next creation request can retry |
| `find(...)`, `list(...)` | `integration-single-process` | manager lookup results are consistent |
| `configure()` handler registration | `integration-single-process` | spot-local registrations like `context.handlers.addPacket(...)`, `context.handlers.addHandler(...)`, `context.handlers.addSubscribe(...)` are reflected in the descriptor |
| Entry Spot actor handler decorator registration | `integration-single-process` | an actor packet handler registered via the `zlinkEntrySpotActorRequestHandler(...)` decorator is reflected in the target Entry Spot registry |
| user Spot actor handler decorator registration | `integration-single-process` | an actor packet handler registered via the `zlinkSpotActorRequestHandler(...)` decorator is reflected in the target user Spot registry |
| Entry Spot packet callback concurrency | `integration-single-process` | the Entry Spot's ordinary packet handler uses the same registration surface as a user Spot but is not serialized against the Entry Spot's entire execution line |
| `onInitialize(...)` handler resolve | `integration-single-process` | the per-spot separate DI scope works normally |
| `onClosing(...)` normal close callback | `integration-single-process` | called exactly once, in the spot execution context, when `close(...)` is called |
| local spot publish | `integration-single-process` | the subscriber receives it normally |
| SPOT timer metadata | `integration-single-process` | the timer handler receives the callback number, scheduled/start time, delay, and skip metadata |
| SPOT timer overrun policy | `integration-single-process` | `SkipLateTicks`, `CatchUpBounded`, `DelayNextTick` each keep the skip, bounded-catch-up, and fixed-delay meanings respectively |
| SPOT timer exception policy | `integration-single-process` | a handler exception is recorded as a monitoring event, and a timer with `stopOnUnhandledException` on stops |
| Entry Spot actor packet dispatch | `contract` | `entry spot actor packets use actor mailboxes without entry-wide serial dispatch` and `entry spot actor handler yield fails immediately instead of timing out` verify the target actor mailbox, same-actor ordering, different-actor progress, and the yield contract error |
| Entry Spot timer execution context | `integration-single-process` | an Entry Spot timer is processed in the same Entry Spot execution queue as the Entry Spot lifecycle callback and request continuation, and the same timer callback is also not run overlapping. Actor packets are processed in the target actor's mailbox |
| SPOT timer cancel | `integration-single-process` | after `cancel()`, the managed timer loop runs no further callbacks |
| outbound-only external publish client | `integration-multi-process` | publish succeeds to the target SPOT[^spot] channel |
| Spot route channel acceptance | `unit` | rejects fanout/dealer-mesh/ambiguous/missing-router/missing-peer-source configurations at startup validation |
| MeshNode manual peer connection | `integration-single-process` | the endpoint from `peerConnections().connect(...)` is applied to RouteMesh peer admission |
| Spot route transport | `integration-single-process` | the MeshNode's ROUTER path delivers routed send/request using the target Spot handle |
| Spot egress target peer selection | `integration-single-process` | selects the target MeshNode peer using the manual connection and the owner RID from location metadata |
| Spot route egress role validation | `unit` | routed Spot egress can only be used with RouteMesh, and it's a startup validation error in a fanout configuration |
| scope cleanup after spot close | `integration-single-process` | no further callback occurs, and dispose also completes normally |
| dispatch context after actor join | `integration-single-process` | an actor handler registered with `ZLinkSpotContext.addHandler(...)` runs in the joined `Spot`'s execution context |
| Entry Spot actor dispatch serialization | `integration-single-process` | Entry Spot actor packets preserve per-actor input order, then are processed in order on the Entry Spot execution queue |
| local actor mailbox dispatch | `integration-single-process` | actor packets that didn't enter a user Spot also follow per-actor mailbox order |
| user Spot actor dispatch serialization | `integration-single-process` | multiple actor packets within the same user Spot are processed in order on the Spot execution queue, protecting Spot state |
| runtime task exception observation | `unit` | exceptions from a detached runtime task or a fire-and-forget handler are observed by the runtime error sink or logger instead of being buried as an unhandled rejection |
| execution queue cancellation semantics | `unit` | queue enqueue/wait cancellation doesn't break the order of, or remove mid-stream, a work item already in the queue |
| Spot handle route path | `integration-single-process` | a routed Spot call verifies the `SpotHandle`'s MeshName, owner RID, and generation before sending the routed message |
| actor manager creation duplicate/type conflict | `integration-single-process` | duplicate `ZLinkActorManager.create(...)` fails with `ActorAlreadyExists`, and an actor type conflict in `getOrCreate(...)` fails with `ActorTypeMismatch` |
| local actor bind creation prohibition | `integration-single-process` | `bind(...)` doesn't call the factory when there's no local actor, and fails with `ActorRouteNotFound` |
| session actor bind: registers a logical actor handle with no fallback | `integration-single-process` | `bind(...)` registers the logical actor handle with no application resolver fallback |
| remote actor dispatch creation prohibition | `integration-single-process` | the routed actor dispatch receive path doesn't call the factory when there's no local actor, and fails the dispatch |
| session actor relay bridge | `integration-single-process` | `bind(...)` and `ZLinkSessionActor.relay(...)` work on the public session surface |
| session actor explicit disconnect notification | `contract`, `integration-single-process` | a session disconnect doesn't automatically propagate to every bound actor — the current Spot actor disconnected handler is called only on `notifyDisconnected(...)` or an explicit runtime call |
| session actor dispatch ordering | `integration-single-process` | a packet relayed from a stream session to an actor guarantees per-actor order, moving through the handler execution path matching the actor's current location |
| actor dispatch location after mailbox wait | `integration-single-process` | after the same actor's preceding packet finishes join, the next waiting packet is dispatched to the new user Spot location, not the previous one |
| session actor dispatch wire multipart | `integration-single-process` | actor dispatch between the Session server and Play server keeps the route header, actor metadata, stream header, and payload as separate parts, without re-serializing the payload as a `Buffer` inside a JSON envelope |
| session actor reconnect reuse | `integration-single-process` | if the same actor id rebinds on a new stream session, the existing actor instance and spot membership are kept, and only the session binding token[^binding-token] is refreshed |
| session actor binding rollback | `integration-single-process` | if the actor-session binding update fails, the helper also fails, and the same token entry is removed from the local binding table |
| stale session binding token guard | `integration-single-process` | a late-arriving unbind or stale bound session message from a previous stream can't erase or use the new binding |
| location-store-based SpotRef resolver registration | `unit` | registering the location store registers the default `ZLinkSpotRefResolver` and the actor spot ref resolver |
| actor-bound session route registration | `integration-single-process` | the actor-session route is saved into actor runtime state on session bind |
| stale session unbind guard | `integration-single-process` | a disconnect arriving with an old binding token doesn't erase the new actor-session binding |
| sample-only-store-free framework/session flow use | `unit` | the TicTacToe.Ts and Bingo.Ts samples use the framework/session flow without a sample-only actor-session store |
| stale bound session send | `integration-single-process` | a one-way push toward an already-closed stream or a stale binding doesn't fail the route receive loop or host shutdown |
| bound session gateway relay | `integration-single-process` | a bound session send from the Play server to the Session server arrives at the client STREAM as a single stream packet through the core session relay binding |
| bound session disconnect local actor | `integration-single-process` | if a local actor calls `ZLinkBoundSession.disconnect(...)` with no actor id, the binding is cleaned up and the session disconnect callback is not called again |
| bound session disconnect remote actor | `integration-single-process` | even if a remote actor calls `ZLinkBoundSession.disconnect(...)` with no actor id, the same close meaning is kept at the session host |
| session context close | `integration-single-process` | `ZLinkSessionContext.close(...)` closes the current stream client's connection from the server side, followed by the disconnect callback |
| packet dispatch right after actor join | `integration-single-process` | a packet arriving after join finishes runs in the new `Spot`'s execution context |
| packet dispatch right after actor spot move | `integration-single-process` | no stale dispatch occurs against the previous `Spot` context |
| spot context channel request path | `integration-single-process` | `spot.context.outbound.requestToChannel(channelName, request)` uses the current MeshNode's logical channel |
| spot context routed send/request surface | `contract`, `integration-single-process` | `ZLinkSpotOutbound`'s Spot/channel send/request and publish use the current MeshNode's route transport |
| actor bound session send API | `integration-single-process` | an actor pushes to the client stream via `context.boundSession.send(...)`, without being directly exposed to `ZLinkStream` |
| actor request handler reply | `unit` | an actor request packet is replied to only via the actor request handler's return value, without falling back to a send handler dispatch. Stream kinds outside send/request are also not treated as actor packets |
| Spot actor request handler reply | `unit` | an Entry Spot/user Spot actor request packet is replied to only via the request handler's return value, without falling back to a send handler dispatch. Stream kinds outside send/request are also not treated as actor packets |
| local actor request relay reply | `integration-single-process` | local session actor relay also writes the stream response from the actor request handler's return value |
| actor context reply not exposed as public surface | `unit` | the actor context reply and actor stream client contract are not re-exposed in the public surface |

## 6. Stream Regression Items

> Mirrors dotnet's `ContractTests/Streams`, `E2ETests/Stream`. Verifies the header-based single
> `onDispatch` session registration, lifecycle callback, and handler invoker surface.

| Item | Layer | Pass Criteria |
|------|------|-----------|
| duplicate session registration on the same node | `unit` | startup validation exception |
| header session node | `integration-single-process` | `onDispatch(...)` call confirmed |
| `onConnected(...)` | `integration-multi-process` | called once, after `ConnectionReady` |
| `onError(...)` scope | `integration-multi-process` | only transport errors are delivered as session callbacks |
| peer metadata surface | `integration-single-process` | `sessionId`, `routingId`, `localAddr`, `remoteAddr` values confirmed |
| session callback task dispatch | `integration-single-process` | the transport callback doesn't call the user callback directly — it calls it through a managed task (microtask queue) path |
| session callback serialization | `integration-single-process` | lifecycle/packet callbacks run serially in the same order as the frames the stream socket preserved for that session, never overlapping in parallel |
| preventing bypass of direct session callback invocation | `unit` | the runtime's internal transport entry point uses only the enqueue API |
| awaiting a handler's `Promise<T>` result | `unit` | the handler invoker converts a generic `Promise<T>` into the actual result value, without a value-type conversion error |
| abstract wire payload validation | `unit` | an abstract/interface payload with no converter, when included in a node-boundary DTO, fails with a configuration error at registration time or right before the first submit |

## 7. Location / Monitoring Regression Items

Verifies the common location runtime and monitoring contract through the Node public surface.

| Item | Layer | Pass Criteria |
|------|------|-----------|
| monitoring source name mismatch | `unit` | startup validation exception |
| location runtime polling diff | `integration-multi-process` | topology, status, service summary events occur |
| spot polling diff | `integration-multi-process` | status, peers, subjects events occur |

## 8. Release Gate

To ship a release, all six of the following must be satisfied. Locally, `npm run verify:release`
runs the required gates below in one go. CI runs the same criteria split into a runtime/ABI matrix job
and a cross-language job.

1. `unit`, `integration-single-process`, `integration-multi-process` all pass
2. `npm run verify:runtime-matrix` passes on both `node20` and `node22`
3. CI gate passes across all six platform ABIs above
4. happy-path samples and representative failure-paths are each covered at least once
5. every disallowed combination defined in the responsible spec is pinned by a test
6. `npm run verify:cross-language` confirms the required cross-language smoke paths pass, showing
   the Node implementation keeps the same wire contract as dotnet/C++/Java

In other words, running the sample once alone is not enough. Startup validation and runtime failure
meaning must also be pinned by tests.

Also, even if the native backend already supports a given platform, the framework stacks
registration, lifecycle, DI, and monitoring layers on top of it. So the platform gate is kept
separate from the backend gate.

## 8.1 Sample / Guide / Cross-Language Release Items

> The usability/sample axis of Phase 9 is owned by the
> [canonical samples](../README.en.md). The items below must run in the release gate.

| Item | Layer | Pass Criteria |
|------|------|-----------|
| `npm run verify:release` | `integration-multi-process` | runs ABI declaration, P0 regression, sample smoke, Node runtime matrix, and cross-language smoke in order |
| `npm run verify:samples` | `integration-multi-process` | all 6 maintained TypeScript samples pass their self-check |
| `npm run verify:runtime-matrix` | `integration-multi-process` | the current runner passes build, typecheck, and the full contract test suite on both Node 20 and Node 22 |
| `npm run verify:abi-matrix` | `unit` | the `framework-node` CI workflow, release docs, and package script keep the same list of `win-x64`, `win-arm64`, `linux-x64`, `linux-arm64`, `darwin-x64`, `darwin-arm64` and Node 20/22 gates |
| `npm run verify:cross-language` | `integration-multi-process` | Node and the dotnet TestHost pass six required channel/stream paths with the same protocol meaning |
| guide chapter map | `unit` | the Node guide's 12 chapters map 1:1 to the dotnet guide's major chapters |
| sample public API import guard | `unit` | samples import only the framework/connector public API and don't directly use the binding internal/native path |
| sample readiness guard | `unit` | samples don't use sleep-only readiness masking and instead wait on observable readiness |
| Node client -> dotnet channel server request/reply | `integration-multi-process` | the dotnet request handler replies with the same payload meaning |
| Node client -> dotnet channel server one-way send | `integration-multi-process` | the dotnet send handler processes it with the same packet meaning |
| Node publisher -> dotnet fanout subscriber publish | `integration-multi-process` | the dotnet publish handler processes it with the same topic/payload meaning |
| dotnet client -> Node channel server | `integration-multi-process` | the Node handler replies to the dotnet client's request with the same payload meaning |
| Browser TypeScript connector -> dotnet stream server | `integration-multi-process` | header session request/reply and notification dispatch work in Chromium |
| dotnet connector -> Node stream server | `integration-multi-process` | the dotnet connector passes through Node's `onDispatch` and `reply` path |

## 8.2 Spot Asynchronous Serial Execution Regression

| Test Case | Pass Criteria |
|---------------|-----------|
| `entry-spot-serial-dispatch.test.js` serial execution item | until a Promise handler finishes, the next work item at the same actor or Spot execution boundary doesn't start. |
| `contract-surface.test.js` call declaration item | actor, bound session, and route call objects don't expose a removed `yield(...)` in the public contract. |
| `npm run build --prefix framework/languages/node/samples/Bingo.Ts` | Bingo.Ts compiles under the `submit(...)` and Promise completion contract, with no public `yield(...)`. |

## 9. Per-Document Regression Test Section

Every implementation-standard document in this directory (and `spec/`) must have a short
`Regression Tests` section describing which tests pin its own items. Updating only the central matrix
isn't enough — a reader of the detailed document can easily miss which tests to look at otherwise.

Just like dotnet's per-document regression tests, Node also confirms that each implementation-standard
document keeps its own regression-test section.

| Test Case | Pass Criteria |
|---------------|-----------|
| `documentation-regression.test.js › node guide exposes the 12 required guide chapters` | all the guide documents below have a `Regression Tests` section. |
| `documentation-regression.test.js › node documentation relative markdown links resolve` | every relative link in the Node documents, including this matrix, resolves. |

> Just as dotnet's narrative guide and case-study documents are excluded from the strict set, Node's
> user guide (usability) layer is also not subject to the strict set. Currently, only the
> implementation-standard documents of the Node bundle (`spec/`, `internals/`, root plan, sample plan)
> are in the strict set.

The target is the Node public contract and implementation-standard documents that currently actually
exist.

- `framework/common/spec/server/languages/node/README.ko.md`
- `framework/common/spec/server/languages/node/interfaces/README.ko.md` and its per-category
  interface documents
- `framework/node/README.ko.md`
- `framework/node/internals/regression-test-matrix.ko.md`
- `../../common/internals/README.ko.md`
- `framework/node/internals/backend-dependency-policy.ko.md`

[^public-contract]: The public contract means the API surface exposed to external users, whose compatibility must be maintained on change.
[^regression]: A regression is a phenomenon where a feature that worked well in a previous version breaks again because of a new change. A regression test is a test bundle that always runs to prevent that.
[^topology]: Topology is the configuration information describing which nodes (channel, spot, registry, etc.) exist where, and how they're connected to each other.
[^abi]: An ABI (Application Binary Interface) combination refers to the OS/CPU architecture combination a Node.js native addon runs on. E.g., `linux-x64`, `darwin-arm64`. The `@zlink-systems/zlink` prebuilt artifact is distributed for these combinations.
[^ci-gate]: A CI gate is the bundle of automated verification steps (build, test, etc.) that must pass before merging or deploying a new change.
[^di]: DI (Dependency Injection) is a pattern where an object doesn't build its own dependencies directly — they're injected by an external container. In NestJS, a module + provider + token-based container is the standard.
[^backpressure]: Backpressure is the mechanism that throttles the send rate so it doesn't exceed the receiving side's processing capacity.
[^hwm]: HWM (High Water Mark) is the maximum number of bytes a send queue can hold — backpressure kicks in once this limit is reached.
[^wire-multipart]: Wire multipart is a way of sending one logical message split into several message parts, such as header and payload. This lets routing work even by looking at just one part.
[^entry-spot]: The Entry Spot is the entry-point Spot that first handles an actor that has entered a MeshNode. It acts as a stage before the actor moves to a user Spot.
[^spot]: `SPOT` is an abstraction that routes messages by dynamically created and destroyed logical node units (e.g., room, stage). A MeshNode hosts one or more Spot instances.
[^binding-token]: A session binding token is a token that identifies the connection state between an actor and a stream session, used to tell which binding is current on reconnect.

---
<!-- framework-adapter-nav:bottom:start -->
[Document List](../README.en.md) | [Previous: Runtime Lifecycle](../../common/internals/README.en.md) | [Next: Backend Dependency Policy](backend-dependency-policy.en.md)
<!-- framework-adapter-nav:bottom:end -->

## Regression Items Moved From The Public Contract Documents

When the per-language spec was compressed into 3 documents (system structure · interfaces ·
connector), the regression items owned by the deleted per-feature contract documents were moved into
this section. The contract's meaning is owned by the common spec.

### Channel

| Test Case | Pass Criteria |
|---------------|-----------|
| `Channels.forRoot_Throws_WhenChannelNameIsDuplicated` | duplicate registration of the same channel name causes a startup validation exception. |
| `Channels.forRoot_Throws_WhenClientHasNoPeerAcquisitionPath` | fails before starting if the client role has no Discovery or manual connection. |
| `ClientServer.ManualClient_Request_And_Send_Work_Across_Hosts` | a manually connected client handles both request and send. |
| `ClientServer.DiscoveryClient_Request_And_Send_Work_Across_Hosts` | a Discovery-based client handles both request and send. |
| `FiltersAndHttp.HttpHandler_Uses_SameContainer_ToResolve_ZLinkChannelClient` | an HTTP controller receives and calls `ZLinkChannelClient` from the same DI container. |
| `channel runtime drains backpressured requests from send-ready callback` | the async submitter drains pending items in the ready callback, without duplicate-sending. |

### SPOT

| Test Case | Pass Criteria |
|---------------|-----------|
| `forRoot throws when spot factory class is duplicated across nodes` | duplicate registration of the same Spot factory class causes a startup validation exception. |
| `forRoot allows standalone local spot node` | a MeshNode configuration using only manual peer connections can start even without a location store. |
| `spotManager create/find/list/close work through framework runtime` | `create`, `find`, `list`, `close`, and scope cleanup are consistent. |
| `spot publish/timer and close stop callbacks work` | timer and publish callbacks run in the Spot lifecycle context, and don't run further after shutdown. |
| `spot timer provides tick metadata` | the timer handler receives the callback number, scheduled/start time, delay, and skip metadata. |
| `spot timer skips late ticks when configured` | the `SkipLateTicks` policy doesn't deliver late ticks without bound — it surfaces them as `skippedTicks`. |
| `spot timer catches up within configured limit` | the `CatchUpBounded` policy only runs consecutively within the `maxCatchUpTicks` cap. |
| `spot timer delayNextTick waits after handler completion` | the `DelayNextTick` policy waits the period again after the handler finishes. |
| `spot timer rejects unknown overrun policy` | an unknown overrun policy value is a configuration error. |
| `spot timer reports handler exception to monitoring` | a handler exception is recorded as a timer failure event in runtime monitoring. |
| `spot timer stopOnUnhandledException stops timer` | a timer with `stopOnUnhandledException` on stops after the first handler exception. |
| `spot timer cancel stops managed timer loop` | after `cancel()`, the managed timer loop runs no further callbacks. |
| `outbound-only spot publisher client publishes to target channel` | an external publisher client publishes to the target SPOT channel. |
| `spot actor join/move/submit run through spot execution context` | actor join, move, and packet dispatch run in the current spot execution context. |
| `entry spot actor packets use actor mailboxes without entry-wide serial dispatch` | an Entry Spot actor packet uses the target actor's mailbox, and different actors don't wait on each other because of the Entry Spot's entire execution line. |
| `entrySpot packet handlers use entrySpot serialization` | the Entry Spot's ordinary packet handler uses a per-Spot serial execution line, just like a user Spot. |
| `entrySpot timer waits for entrySpot callbacks` | an Entry Spot timer callback doesn't run concurrently with another callback of the same Entry Spot. |
| `entrySpot timer does not reenter same timer` | an Entry Spot timer doesn't run the same timer callback overlapping. |

### Actor

| Test Case | Pass Criteria |
|---------------|-----------|
| --- | --- |
| `actorFactoryNameIsDuplicated` | a duplicate actor factory name (actorType) is blocked with an exception at startup validation. |
| `entrySpotAndUserSpotActorPacketRegistriesDispatch` | actor packet handlers and disconnected handlers registered on Entry Spot and user Spot dispatch normally. |
| `entry spot callbacks from mixed setImmediate/queueMicrotask backend callbacks keep enqueue order without overlap` | Entry Spot callbacks run in order on the Entry Spot execution line regardless of the backend task context they arrive in. |
| `entry spot does not start the next callback before the previous handler promise settles` | the same Entry Spot's next callback doesn't start before the previous Entry Spot handler's Promise settles. |
| `spotActorJoinMoveAndSubmitRunThroughSpotExecutionContext` | after an actor moves spots, dispatch doesn't happen against the stale spot context. |
| `sessionActorDispatchRelaysStreamRequestAndRoutesBySequence` | a request is relayed from a stream session to a bound actor, and the reply order per sequence is correct. |
| `localSessionActorDispatchRepliesFromRequestHandler` | local actor relay also writes the stream response from the request handler's return value. |
| `spotActorRegistryDoesNotResolveRequestToSendHandler` | an Entry Spot/user Spot actor request packet isn't dispatched to a send handler as a fallback, and stream kinds outside send/request also aren't treated as actor packets. |
| `publicSurfaceRemovesActorReplyAndStreamClientContracts` | the actor context reply and actor stream client contract are not re-exposed in the public surface. |

### STREAM

| Test Case | Pass Criteria |
|---------------|-----------|
| `nodesAndServices.throwsWhenStreamNodeRegistersMultipleSessions` | duplicate session registration on the same node causes a startup validation exception. |
| `ZLinkModule.forRoot maps stream node options into runtime registration` | even at the Node builder surface, registering a session twice on the same stream node causes a startup validation exception. |
| `protocol.streamSessionRuntimeOnlyExposesEnqueueCallbackEntrypoints` | the transport entry point exposes only the public enqueue API. |
| `stream session node runtime does not invoke user callbacks inside transport callback` | the transport callback doesn't run the user's `onDispatch(...)` directly in the same call stack — it hands it off to the managed queue. |
| `headerStreamSession.receivesRepliesAndTracksLifecycle` | connected, dispatch, reply, metadata, disconnected/error callbacks run in the expected order. |
| `headerStreamSession.canCloseCurrentClientStream` | the session context can close the current client stream from the server side. |
| `stream session and bound session require packetName for structural payloads` | a structural payload isn't sent without an explicit packet name, on either the stream session send or bound session send side. |

### Monitoring

| Test Case | Pass Criteria |
|---------------|-----------|
| `socket monitoring source maps backend raw events into framework typed events` | converts socket backend events into the framework's typed events. |
| `location runtime monitoring source publishes snapshot changes and suppresses unchanged polls` | delivers status/topology/service summary events only when the location runtime snapshot changes. |
| `location monitoring event emitter publishes registered row and resolve-miss events` | delivers location row registration and resolve failures as typed events. |
| `spot monitoring source publishes status peers and subjects snapshot changes` | delivers Spot's status/peer/subject snapshot changes as typed events. |
| `spot timer reports handler failure immediately through runtime publisher` | delivers timer handler exceptions immediately through the runtime publisher. |
| Common Concept | Node Type / Member |
| Log Mode | `ZLinkMessageFlowLogMode` { `Off`, `ErrorsOnly` (default), `KeyTransitions`, `Verbose` } |
| outcome | `ZLinkMessageFlowOutcome` { `succeeded`, `failed`, `backpressured`, `dropped`, `cancelled`, `shutdown` } |
| event | `ZLinkMessageFlowEvent`: `eventId`, `outcome`, `surface`, `messageKind`, `phase?`, `packetName?`, `meshName?`, `channelName?`, `topic?`, `correlationId?`, `sourceRid?`, `targetRid?`, `spotRid?`, `actorId?`, `messageSizeBytes?` |
| observer | `ZLinkMessageFlowObserver.onMessageFlow(flow): Promise<void> \| void` |
| diagnostic options | `ZLinkDiagnosticsOptions` { `messageFlow`, `sampleRate`, `includeMessageSizes`, `logFile?`, `label?` } |
| runtime toggle | host `ZLinkMessageFlowControl.setMessageFlowMode(mode)` / `messageFlowMode()` |
| Common Concept | Node.js |
| meter name (constant) | `ZLinkMeters.Framework` = `'zlink.framework'` |
| instrument emission | OpenTelemetry Metrics API `Meter` — `Counter`/`UpDownCounter`/`ObservableGauge`/`Histogram` |
| app wiring (common case) | global OTel `MeterProvider` (SDK) configuration — no separate zlink setting |
| custom (optional) | inject a provider with `ZLinkModule.forRoot(zlinkFramework().options({ metrics: { meterProvider } }).build())` |
| Common Concept | Node.js |
| creation gate | uses the existing `configureDispatch().messageFlow(...)` setting as-is. There's no separate flow id setting. |
| event field (added) | `readonly flowId: string`, `readonly flowOrigin: ZLinkFlowOrigin` — the same root value even on error events |
| Common Concept | Node.js |
| automatic termination (default) | the framework joins an in-progress host shutdown or starts `Shutdown` in `onApplicationShutdown()` |
| `Shutdown` order | block new application acceptance → complete already-accepted execution turns and requests → confirm in-progress relocation/STREAM barrier → clean up local object/ownership/peer resources → forced termination within a bound, if needed |
| `Retire` order | all-or-none preflight → target reservation → admission seal → Actor/Instance Spot continuity relocation → STREAM barrier → host resource cleanup |
| Spot re-creation boundary | public `create`/`getOrCreate` are local-only. Only Instance address cold activation and explicit `Retire` target materialization run under a separate contract, and a stale handle doesn't start a hidden remote create |
| explicit control | the host singleton `ZLinkFrameworkRuntime`'s `retire(options?)` and `shutdown(options?)`; the default deadline is 30,000ms, and `AbortSignal` ends only the waiter |
| termination result | `ZLinkTerminationResult` provides the effective intent, a `Stopped|Blocked|ForceStopped` outcome, and a closed reason together |
| readiness probe | the framework provides NestJS Terminus's `ZLinkDrainHealthIndicator(runtime, meshName)` to confirm a specific MeshName's readiness. Or query `ZLinkRouteMeshRuntime.isReady(meshName)` directly |
| Mesh drain result | `ZLinkRouteMeshRuntime.drain(...)` and `awaitDrained(...)` return a `ZLinkMeshDrainResult`. Forced-termination reasons are limited to the four values `deadline_exceeded`, `drain_state_publish_failed`, `owner_cleanup_failed`, `teardown_failed` |
| status observation | host shutdown is confirmed via `zlink.runtime.host.termination_changed` from `ZLinkFrameworkRuntime.observe(...)`, and Mesh projection via `ZLinkRouteMeshRuntime.observe(...)` |

### Session Actor Dispatch

| Test Case | Pass Criteria |
|---------------|-----------|
| `RemoteSessionRelayTests.SessionActorDispatch_Relays_Stream_Request_And_Routes_Request_To_Bound_Actor_By_Sequence` | relays an actor request from a session callback, and returns the reply through the request sequence. |
| `ActorDisconnectNotifyTests.ClientClose_Cleans_Session_Without_Actor_Disconnect_Callback` | closing a client stream only cleans up the session binding — it doesn't call the Actor disconnect callback. |
| `ActorBindingTests.BindActorAsync_DoesNot_Create_LocalActor` | logical actor binding doesn't create a new local actor during session attach. |
| `ActorBindingTests.SessionActorBind_WithoutRoute_Is_LocalOnly` | the bind overload with no route attaches only to the local actor and doesn't perform a remote fallback. |
| `RemoteProxyDisconnectTests.BoundSessionDisconnect_FromRemoteActor_Closes_Client_Without_Session_Disconnect_Callback` | even if a remote actor calls `boundSession.disconnect(...)`, the same close meaning is kept at the session host. |
| `entry spot callbacks from mixed setImmediate/queueMicrotask backend callbacks keep enqueue order without overlap` | even if backend callbacks arrive from different task contexts, they run on the Entry Spot execution line in registration order without overlapping. |
| `entry spot does not start the next callback before the previous handler promise settles` | the same Entry Spot's next callback doesn't start before the handler's Promise settles. |
| `entry spot actor packets use actor mailboxes without entry-wide serial dispatch` | an Entry Spot actor packet uses the target actor's mailbox, and different actors don't wait on each other because of the Entry Spot's entire execution line. |
| `LocalActorMailboxExecutionTests.LocalActorPackets_Are_Serialized_Per_Actor_And_Parallel_Across_Actors` | actor packets that didn't enter a user Spot keep per-actor order while being able to run in parallel across different actors. |
| `ActorRegistryExecutionTests.ActorDispatch_Rechecks_CurrentLocation_After_Waiting_For_ActorMailbox` | after the same actor's earlier packet finishes join, the next waiting packet is dispatched to the new user Spot location. |
| `ActorLifecycleTests.SpotActorJoin_Move_And_Submit_Run_Through_SpotExecutionContext` | dispatch after actor join runs in the current spot execution context. |
| `ActorSessionStateTests.ActorSessionState_Filters_StaleDisconnect_And_Only_Disconnects_CurrentStream` | a late disconnect from a previous stream doesn't disconnect the current actor-session connection. |
| `HeaderStreamSessionTests.HeaderStreamSession_Can_Close_Current_Client_Stream` | the session context closes the current client stream, naturally followed by the disconnect callback. |
| `SerialExecutorTests.StreamSessionSerialExecutor_Continues_After_Work_Exception` | a fire-and-forget work exception in the session queue is recorded in the error sink and doesn't block the next work item. |
| `SerialExecutorTests.SpotSerialExecutor_Continues_After_Queued_Work_Exception` | a fire-and-forget work exception in the Spot queue is recorded in the error sink and doesn't block the next work item. |
| `runtime task runner observes detached task exceptions without unhandled rejection` | the Node runtime task runner observes detached task exceptions without creating an unhandled rejection. |
| `framework runtime state aborts listener tasks before disposing backend context` | runtime state shutdown delivers a stop signal to listener tasks first, cleaning up the backend context last. |
| `SerialExecutorTests.SpotSerialExecutor_ExecuteAsync_Propagates_Work_Exception` | the completion-waiting execution path in the Spot queue returns the handler exception straight to the caller. |
| `SerialExecutorTests.SerialExecutionQueue_RunAsync_Propagates_Work_Exception` | the common serial queue's `run(...)` records a work exception to the error sink while also propagating it to the caller. |
| `SerialExecutorTests.SerialExecutionQueue_Wait_Cancellation_Does_Not_Remove_Queued_Work` | even if a completion wait is canceled in the common serial queue, a work item already in the queue is not removed. |
| `SerialExecutorTests.ActorDispatchCancellation_Does_Not_Stop_Current_Or_Later_Dispatch` | canceling an actor dispatch wait doesn't stop the currently running dispatch or a later dispatch. |
| `RegressionTests.NodeSessionActorDispatch_Documents_ExecutionSerialization_Core_Code` | the execution-serialization core-code section keeps describing the queue, runtime task, error sink, and cancellation meaning. |
| `RegressionTests.NodeRegressionMatrix_Includes_ExecutionSerialization_Guards` | the central regression matrix keeps its execution-serialization regression items. |

### MeshNode And Spot

| Test Case | Pass Criteria |
|---------------|-----------|
| `EntryRoutingTests.EntrySpotRoutingId_IsApplied_ToNativeEntrySpot` | the routing id specified by `entrySpot.routingId` is applied to the native Entry Spot facade and exposed as the `spotRid` of Entry Spot activation. |
| `LocationRuntimeTests.SpotRefResolver_Resolves_Created_Spot_By_Rid_And_Removes_Route` | the Spot RID route is used as an index that finds only by Spot rid, and the resolver preserves the owner node rid and `SpotKind.User` of a valid spot location row. |
| `ManagerTests.SpotManager_Create_List_Close_And_Publish_Work_Through_FrameworkRuntime` | `create`, `find`, `list`, `close`, and scope cleanup work consistently. |

### Stage Wrapper

| Test Case | Pass Criteria |
|---------------|-----------|
| `E2E:SM-B7` | after an actor join, packets in the stage-role Spot are processed in the right lifecycle order. |
| `E2E:SM-E3` | a timer used as the stage tick doesn't create further callbacks after the Spot shuts down. |
| `E2E:SM-A5` | the application stage wrapper runs Spot requests, timers, and lifecycle through the public API. |

### Bootstrap/Overview

| Test Case | Pass Criteria |
|---------------|-----------|
| `backend-contract.test.js` | the backend adapter factory provides all of the channel, Spot, stream, and monitoring adapters. |
| `backend-public-api-only.test.js` | the framework runtime doesn't directly import the binding internal/native path. |
| `nestjs-module.test.js` | `ZLinkModule.forRoot/forRootFactory`, provider token exposure, startup validation, injection into an actual NestJS application context, and lifecycle wiring all work. |
| `documentation-regression.test.js › node implementation reference docs declare regression coverage sections` | this overview keeps its own regression-test section. |
