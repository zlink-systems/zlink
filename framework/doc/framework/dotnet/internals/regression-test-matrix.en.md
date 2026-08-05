<!-- framework-adapter-nav:start -->
[Document List](../../../README.en.md) | [Previous: Runtime Execution](../../common/internals/02-serialization.en.md) | [Next: Backend Dependency Policy](backend-dependency-policy.en.md)
<!-- framework-adapter-nav:end -->

[Spec Index](../../common/README.en.md)

[.NET Bundle](../README.en.md) | [Runtime Lifecycle](../../common/internals/README.en.md) | [Runtime Execution](../../common/internals/02-serialization.en.md) | [Backend Policy](backend-dependency-policy.en.md) | [Common E2E](../../common/e2e/README.en.md)

# ZLink Framework .NET Regression Test Matrix

## 1. Purpose

This document lays out, test item by test item, what counts as a regression when the implementation
changes. The meaning of public behavior is owned by the responsible spec; this document owns only the
tests that verify that contract and the release gate.

## 2. CI Layers

Regression tests are split into the following three layers.

| Layer | Purpose | Example |
|------|------|------|
| `unit` | registration validation, dispatch lookup, option parsing | duplicate registration, builder validation |
| `integration-single-process` | confirms runtime combinations work normally within the same host | channel request/send, in-memory location store, monitoring attach |
| `integration-multi-process` | confirms actual topology[^topology] and reconnect behavior | location runtime query, store row change, spot peer change |

## 3. Minimum CI Matrix

| Item | Standard |
|------|------|
| target framework | `net8.0`, `net10.0` |
| runtime RID[^rid] | `win-x64`, `win-arm64`, `linux-x64`, `linux-arm64`, `osx-x64`, `osx-arm64` |
| test mode | debug, release |

The repository's current default build (the `ZLinkFrameworkTargetFrameworks` default) is a single
`net8.0` TFM, so regression tests run on `net8.0`. `net10.0` is additionally handled in the
multi-target build for regression-matrix reporting, as below.

- The repository's current default build is a single `net8.0` TFM.
- `net10.0` is handled as an extra compile/run in the multi-target build for regression-matrix
  reporting.

Meanwhile, the packaging target of the repository's `bindings/dotnet/runtimes/` and the native
artifact combination `.github/workflows/build.yml` produces are both based on the six runtime RIDs
above. The framework CI gate[^ci-gate] also treats this as the default scope by default.

In other words, `.NET` framework regression tests don't run just one representative OS and call it
done. Under the current plan, the platforms that must pass are:

- Windows x64
- Windows ARM64
- Linux x64
- Linux ARM64
- macOS x64
- macOS ARM64

## 4. Channel Regression Items

| Item | Layer | Pass Criteria |
|------|------|-----------|
| duplicate fanout channel name registration | `unit` | startup validation exception |
| `AddFanoutChannel(...).EnableSubscriber(endpoint)` | `integration-single-process` | manual-based subscribe succeeds |
| no peer acquisition path on a subscriber role | `unit` | startup validation exception |
| a subscriber with a location store specifying a manual endpoint | `unit` | that subscriber uses the manual connection, without affecting another role's automatic connection |
| no bind endpoint on a publisher role | `unit` | startup validation exception |
| publisher-only channel | `integration-single-process` | publish submit succeeds |
| subscriber location-store attach | `integration-multi-process` | remote publish is received |
| handler group mapping | `unit` | `AddZLinkHandlers...()` alone doesn't become a global dispatch target — only handlers of a group mapped by `channel.AddHandlerGroup("...")` are dispatched on that channel |
| empty fanout subscriber validation | `unit` | a fanout subscriber with no publish handler exposure isn't allowed as an empty receiver — it's a startup validation error |
| typed handler registration | `unit` | a handler registered directly through the channel's `Add...Handler(...)` is exposed to that channel even without group mapping |
| channel type handler compatibility | `unit` | a fanout subscriber allows only a publish handler, and a MeshNode ChannelName allows only a send/request handler |
| incompatible handler group mapping | `unit` | if a handler mismatched with the channel type is mixed into a group, it fails with a startup validation error instead of excluding only that part |
| route mesh handler group mapping | `integration-single-process` | a route mesh channel's `AddHandlerGroup(...)` exposes the route send/request handler group as an actual routed dispatch target |
| duplicate handler on the same ChannelName | `unit` | startup validation exception if two or more handlers share the same `kind + packetName` |
| the same packet handler on a different ChannelName | `integration-single-process` | mapping the same `kind + packetName` to different ChannelNames still dispatches each namespace independently |
| mapping the same group to multiple channels | `integration-single-process` | exposing the same `[ZLinkHandlerGroup("api")]` to two channels via `AddHandlerGroup` still keeps the dispatch namespace independent per channel |
| `AddHandlerGroup` pointing to a nonexistent group | `unit` | a startup validation error if the mapped group has no handler at all |
| event handler group mapping | `unit` | only the publish handler of the group mapped via `channel.AddHandlerGroup("...")` is dispatched on that subscriber channel |
| using `IZLinkRouteClient` from an HTTP handler | `integration-single-process` | works normally in the same DI[^di] container as a route handler |
| using `IZLinkRouteClient` from a ChannelName handler | `integration-single-process` | a request handler requests a different ChannelName with the same DI container's client, and receives a reply from the target the process-local send path selected |
| fanout publish from a channel handler | `integration-single-process` | an ordinary request handler publishes a fanout event with the same DI container's `IZLinkFanoutClient`, and a subscriber handler receives it |
| send async submit backpressure[^backpressure] | `integration-single-process` | doesn't block the caller thread even when the HWM[^hwm] is reached, completing after ready |
| publish async submit backpressure | `integration-single-process` | doesn't block a thread under a ROUTER HWM condition — completes or fails according to the `SendTimeout` policy |
| separating request submit/reply timeout | `integration-single-process` | a request packet's submit delay is judged by `SendTimeout`, and the reply wait by `Timeout(...)` |
| pending request cleanup | `unit` | the request sequence is removed from the pending map on submit failure, timeout, cancellation, or runtime stop |
| ready callback batch drain | `integration-single-process` | once the socket is ready, pending sends/publishes are processed as a batch, without duplicate-sending the same frame |
| channel wire multipart[^wire-multipart] | `integration-single-process` | server-to-server channel send/request/reply sends `header` and `payload` as separate message parts, and handler dispatch selects the packet by looking only at the header part |
| publish wire multipart | `integration-single-process` | `PUB/SUB` publish also keeps the framework header and payload as separate parts, and only the typed payload is delivered to the subscriber handler |

## 4.1 Dispatch Error Observer Regression Items

| ID | Layer | Test Location | Pass Criteria |
|----|------|-------------|-----------|
| DERR-001, DERR-007, DERR-011, DERR-014 | `unit` | `Zlink.Framework.UnitTests/Runtime/UnhandledDispatchPolicyTests.cs` | A missing channel request handler ends in an error reply plus observer event, a missing channel send handler ends in a drop plus observer event, and an observer exception doesn't break the original dispatch result |
| DERR-002, DERR-008 | `unit` | `Zlink.Framework.UnitTests/Runtime/UnhandledDispatchPolicyTests.cs` | A missing route request handler ends in an error reply; a missing route send handler ends in a drop, with an observer event left behind |
| DERR-003, DERR-004, DERR-009, DERR-010, DERR-016 | `unit` | `Zlink.Framework.UnitTests/Runtime/UnhandledDispatchPolicyTests.cs` | A SPOT route, subscription, or actor dispatch failure ends in an error reply or caller-visible error for a request, or a drop plus observer event for one-way |
| DERR-005, DERR-006, DERR-013, DERR-015 | `unit` | `Zlink.Framework.UnitTests/Runtime/UnhandledDispatchPolicyTests.cs` | Decode failures and handler exceptions end in an error reply or an observable drop, with the default log and metric still recorded even without an observer registered |

## 4.2 DI Capability Regression Items

| Item | Layer | Pass Criteria |
|------|------|-----------|
| an actor factory with no Object Server | `unit` | an actor factory can only be added within an Object Server registration |
| actor manager without Object runtime | `unit` | `IZLinkActorManager` is absent from DI if there's no Object Client/Server configuration |
| actor manager without actor factory | `unit` | `IZLinkActorManager` is absent from DI if there's only Object runtime with no actor factory |
| actor manager with actor factory | `unit` | `IZLinkActorManager` is registered in DI when both Object runtime and the actor factory exist |
| Spot service without Object runtime | `unit` | `IZLinkSpotManager` is absent from DI if there's no Object Client/Server configuration |
| Spot service with Object runtime | `unit` | the Spot service is registered in DI when Object runtime exists |
| Spot publisher without a publisher role | `unit` | even with Object runtime, the Spot publisher service is absent from DI without a publisher role |
| Spot publisher with a publisher role | `unit` | `IZLinkSpotPublisherClient` is registered in DI when the Spot publisher role exists |
| bound session factory registration | `unit` | `IZLinkBoundSessionFactory` is registered together with the framework runtime |
| missing ChannelName send path | `unit`, `contract` | `IZLinkRouteClient.SendToChannel(...)` and `RequestToChannel(...)` take only a ChannelName, with no MeshName argument, ending in `NotFound` if there's no registered process-local send path |

## 5. Spot Regression Items

| Item | Layer | Pass Criteria |
|------|------|-----------|
| duplicate Spot factory type | `unit` | startup validation exception |
| duplicate `AddEntrySpot<TEntrySpot>()` | `unit` | startup validation exception on duplicate Entry Spot registration on the same RouteMesh Object Server |
| `AddRouteMesh` call | `integration-single-process` | the MeshNode builder owns both Channel membership and Object Client/Server registration together |
| local-only spot factory with no location store | `integration-single-process` | starts a single-process Object Server runtime with no store endpoint |
| `IZLinkSpotManager.Create(stableType).Async()` | `StatefulServiceRuntimeTests.PublicSpotManagerUsesReserveAndRemoteUserSpotCommands` | the Framework selects an eligible Object Server and issues a global SpotId. The result's `SpotRef`, create `State`, and reply match the authority result |
| `Create(stableType).Async()` with no payload | `integration-single-process` | creation with no payload also calls `IZLinkSpot.OnCreateAsync(...)` exactly once with an empty `ZLinkMessage` |
| `Create(stableType).Request(request).Async()` | `integration-single-process` | the create request `ZLinkMessage` is delivered exactly once to `IZLinkSpot.OnCreateAsync(...)` |
| `GetOrCreate(spotId, stableType).Async()` existing | `integration-single-process` | if the same SpotId is already Ready, it's `State = Existing`, and the new request is not delivered to `OnCreateAsync(...)` |
| concurrent `GetOrCreate(spotId, stableType).Async()` | `integration-single-process` | concurrent creates of the same SpotId commit only one authority creation. Each caller receives that operation's terminal result |
| `GetOrCreate(...)` of the same stable type | `integration-single-process` | returns the existing SpotRef without calling `OnCreateAsync(...)` again |
| `Create(stableType).Request(request).Async()` rejected | `integration-single-process` | a rejection from `OnCreateAsync(...)` returns `State = Rejected` and a reply `ZLinkMessage`, and the Spot authority is not registered as Ready |
| spot create lifecycle failure | `integration-single-process` | a reject from `OnCreateAsync(...)` returns `State = Rejected`, and an exception from `OnCreateAsync(...)` or `OnInitializeAsync(...)` propagates as `InternalFailure`, with the failed entry removed so the next creation request can retry |
| `FindAsync(spotId)` | `StatefulServiceRuntimeTests.FrameworkHostAutomaticallyExecutesRemoteUserSpotCreateAndCloseAgainstAuthorityStore` | returns the current SpotRef for a global SpotId, and `null` after close |
| `Configure()` handler registration | `integration-single-process` | registrations like `Context.Handlers.AddPacket(...)`, `AddActorPacket(...)`, `AddSubscribe(...)` are reflected in the frozen handler catalog |
| Entry Spot handler registration | `integration-single-process` | `Context.Handlers` registered via `AddEntrySpot<TEntrySpot>()` and the actor membership lifecycle callback are reflected in the Entry Spot catalog |
| Entry Spot packet callback concurrency | `integration-single-process` | the Entry Spot's ordinary packet handler uses the same registration surface as a user Spot but is not serialized against the Entry Spot's entire execution line |
| `OnInitializeAsync(...)` handler resolve | `integration-single-process` | the per-spot separate DI scope works normally |
| `OnClosingAsync(...)` normal close callback | `integration-single-process` | called exactly once, in the spot execution context, when `CloseAsync(...)` is called |
| `IZLinkSpotContext.CloseAsync(...)` self close | `integration-single-process` | requesting closure of the current Spot during a timer/handler run proceeds with close after the current callback, and disappears from manager lookups |
| `CloseAsync(...)` with joined actors | `integration-single-process` | a user Spot with joined actors remaining rejects close, returning `false` |
| local spot publish | `integration-single-process` | the subscriber receives it normally |
| SPOT timer metadata | `integration-single-process` | the timer handler receives the callback number, scheduled/start time, delay, and skip metadata |
| SPOT timer overrun policy | `integration-single-process` | `SkipLateTicks`, `CatchUpBounded`, `DelayNextTick` each keep the skip, bounded-catch-up, and fixed-delay meanings respectively |
| SPOT timer exception policy | `integration-single-process` | a handler exception is recorded as a monitoring event, and a timer with `StopOnUnhandledException` on stops |
| Entry Spot timer execution context | `integration-single-process` | Entry Spot timer consistency is verified separately from actor packet mailbox work. The same timer callback is not run overlapping |
| SPOT timer cancel | `integration-single-process` | after `CancelAsync()`, the managed timer loop runs no further callbacks |
| outbound-only external publish client | `integration-multi-process` | publish succeeds to the target SPOT[^spot] channel |
| Spot route authority lookup | `unit`, `integration-multi-process` | the caller passes only the global SpotId, and the Framework looks up the current owner and generation from the Location Store |
| Spot route transport | `unit` | the Framework sends the send/request over the resolved owner's RouteMesh connection. The Application doesn't specify an egress channel or target NodeRid |
| Instance Spot cold activation | `integration-multi-process` | when there's no authority, only a send/request that specifies `InstanceSpot(type)` selects an eligible Object Server, creates the Spot, and processes the first message in that Spot's queue |
| Spot route Object Client role validation | `unit` | a process using an external Spot send/request registers the Object Client role for that Mesh |
| scope cleanup after spot shutdown | `integration-single-process` | no further callback occurs, and dispose also completes normally |
| dispatch context after actor join | `integration-single-process` | an actor handler registered with `IZLinkSpotContext.Handlers.AddActorPacket(...)` runs in the joined Spot's execution context |
| Entry Spot actor mailbox dispatch | `EntrySpotActorDispatchTests.EntrySpotActorDispatch_ConcurrentActors_StartsOutsideEntrySpotSerialLine_AndKeepsSameActorOrdering` | Entry Spot actor packets preserve per-actor input order, and different actors' handler starts are not blocked by the Entry Spot execution queue |
| local actor mailbox dispatch | `integration-single-process` | actor packets that didn't enter a user Spot also follow per-actor mailbox order |
| user Spot actor dispatch serialization | `integration-single-process` | multiple actor packets within the same user Spot are processed in order on the Spot execution queue, protecting Spot state |
| runtime task exception observation | `unit` | exceptions from a detached runtime task or a fire-and-forget handler are observed by the runtime error sink or logger instead of being buried as an unobserved exception |
| execution queue cancellation semantics | `unit` | queue enqueue/wait cancellation doesn't break the order of, or remove mid-stream, a work item already in the queue |
| Spot message follow | `integration-multi-process` | during relocation, a Spot/Actor message arriving at the previous owner is relayed to the current owner, without exposing a stale route to the application |
| actor manager creation duplicate/type conflict | `integration-single-process` | duplicate creation via `IZLinkActorManager.Create(actorId, actorType).Async(...)` and a type conflict in `GetOrCreate(actorId, actorType).Async(...)` respect the public result/typed error contract |
| local actor bind creation prohibition | `integration-single-process` | `BindAsync(...)` doesn't call the factory when there's no local actor, and fails with `NotFound` |
| no resolver fallback for session actor bind | `integration-single-process` | `BindAsync(...)` registers the logical actor handle with no application resolver fallback |
| remote actor dispatch creation prohibition | `integration-single-process` | the routed actor dispatch receive path doesn't call the factory when there's no local actor, and fails the dispatch |
| session actor relay bridge | `integration-single-process` | `BindAsync(...)` and `IZLinkSessionActor.RelayAsync(...)` work on the public session surface |
| session actor explicit disconnect notification | `contract`, `integration-single-process` | a session disconnect doesn't automatically propagate to every bound actor — the current Spot's `OnDisconnectActorAsync(...)` callback is called only on `NotifyDisconnectedAsync(...)` or an explicit runtime call |
| session actor dispatch ordering | `integration-single-process` | a packet relayed from a stream session to an actor guarantees per-actor order, moving through the handler execution path matching the actor's current location |
| actor dispatch location after mailbox wait | `integration-single-process` | after the same actor's preceding packet finishes join, the next waiting packet is dispatched to the new user Spot location, not the previous one |
| session actor dispatch wire multipart | `integration-single-process` | actor dispatch between the Session server and Play server keeps the route header, actor metadata, stream header, and payload as separate parts, without re-serializing the payload as a `byte[]` inside a JSON envelope |
| session actor reconnect reuse | `integration-single-process` | if the same actor id rebinds on a new stream session, the existing actor instance and spot membership are kept, and only the session binding token[^binding-token] is refreshed |
| session actor binding rollback | `integration-single-process` | if the actor-session binding update fails, the helper also fails, and the same token entry is removed from the local binding table |
| stale session binding token guard | `integration-single-process` | a late-arriving unbind or stale bound session message from a previous stream can't erase or use the new binding |
| Spot route location lookup | `unit` | with no separate public resolver SPI, the Framework looks up the global SpotId's current authority from the Location Store |
| actor-bound session route registration | `integration-single-process` | the actor-session route is saved into actor runtime state on session bind |
| Location Store provider substitution | `unit`, `contract` | the Framework uses the application-registered `IZLinkLocationStore` as the authority storage implementation of the two public Store SPIs, without exposing a separate resolver SPI |
| Location Store SpotId route | `StatefulServiceRuntimeTests.FrameworkHostAutomaticallyExecutesRemoteUserSpotCreateAndCloseAgainstAuthorityStore` | finds the Spot created by `IZLinkSpotManager.Create(stableType).Async(...)` by global SpotId, and returns not found after shutdown |
| stale session unbind guard | `integration-single-process` | a disconnect arriving with an old binding token doesn't erase the new actor-session binding |
| working with no actor-session store | `unit` | the Bingo sample uses actor-bound sessions with no actor-session store |
| no SessionGateway variant | `unit` | the TicTacToe SessionGateway variant no longer remains in the sample tree or solution |
| stale bound session send | `integration-single-process` | a one-way push toward an already-closed stream or a stale binding doesn't fail the route receive loop or host shutdown |
| bound session gateway relay | `integration-single-process` | a bound session send from the Play server to the Session server arrives at the client STREAM as a single stream packet through the core session relay binding |
| bound session disconnect local actor | `integration-single-process` | if a local actor calls `IZLinkBoundSession.DisconnectAsync(...)` with no actor id, the binding is cleaned up and the session disconnect callback is not called again |
| bound session disconnect remote actor | `integration-single-process` | even if a remote actor calls `IZLinkBoundSession.DisconnectAsync(...)` with no actor id, the same close meaning is kept at the session host |
| session context close | `integration-single-process` | `IZLinkSessionContext.CloseAsync()` closes the current stream client's connection from the server side, followed by the disconnect callback |
| packet dispatch right after actor join | `integration-single-process` | a packet arriving after join finishes runs in the new `Spot` execution context |
| packet dispatch right after actor spot move | `integration-single-process` | no stale dispatch occurs against the previous `Spot` context |
| spot context channel request path | `unit` | `Spot.Context.Outbound.RequestToChannel(channelName, ...)` uses a process-local ChannelName send path with no MeshName argument |
| spot context routed send/request surface | `contract`, `integration-single-process` | `IZLinkSpotOutbound` exposes `SendToSpot`, `RequestToSpot`, `Publish`, `SendToChannel`, `RequestToChannel` all together. `Publish`/channel send/request use ChannelName, and Spot direct looks up the current authority of the global SpotId |
| actor bound session send API | `integration-single-process` | an actor pushes to the client stream via `Context.BoundSession.Send(...)`, without being directly exposed to `IZLinkStream` |
| actor request handler reply | `unit` | an actor request packet is replied to only via the actor request handler's return value, without falling back to a send handler dispatch. Stream kinds outside send/request are also not treated as actor packets |
| Spot actor request handler reply | `unit` | an Entry Spot/user Spot actor request packet is replied to only via the request handler's return value, without falling back to a send handler dispatch. Stream kinds outside send/request are also not treated as actor packets |
| local actor request relay reply | `integration-single-process` | local session actor relay also writes the stream response from the actor request handler's return value |
| no actor reply public API surface | `unit` | the actor context Reply and actor stream client contract are not re-exposed in the public API surface |

## 6. Stream Regression Items

The exact lifecycle, dispatch, transport, and observer contract of the separate client package
follows the [.NET Stream Connector Public Contract](../../common/spec/stream-connector/languages/dotnet/03-stream-connector.en.md).

| Item | Layer | Pass Criteria |
|------|------|-----------|
| duplicate session registration on the same node | `unit` | startup validation exception |
| header session node | `integration-single-process` | `OnDispatchAsync(...)` call confirmed |
| `OnConnectedAsync(...)` | `integration-multi-process` | called once, after `ConnectionReady` |
| `OnErrorAsync(...)` scope | `integration-multi-process` | only transport errors are delivered as session callbacks |
| peer metadata surface | `integration-single-process` | `SessionId`, `RoutingId`, `LocalAddr`, `RemoteAddr` values confirmed |
| session callback task dispatch | `integration-single-process` | the transport callback doesn't call the user callback directly — it calls it through a managed task path |
| session callback serialization | `integration-single-process` | lifecycle/packet callbacks run serially in the same order as the frames the stream socket preserved for that session, never overlapping in parallel |
| preventing bypass of direct session callback invocation | `unit` | the runtime's internal transport entry point uses only the enqueue API |
| awaiting a handler's `ValueTask<T>` result | `unit` | the handler invoker converts a generic `ValueTask<T>` into the actual result value, without a value-type boxing error |
| abstract wire payload validation | `unit` | an abstract/interface payload with no converter, when included in a node-boundary DTO, fails with a configuration error at registration time or right before the first submit |

## 7. Location / Monitoring Regression Items

The `.NET` public input rules for automatic routing id allocation follow
[Configuration And Topology Exact Interface](../../common/spec/server/languages/dotnet/interfaces/03-configuration-topology.en.md).
Descriptor claim and startup order are described in
[Layering Boundaries And Identifiers](../../common/internals/01-layering.en.md).

| Item | Layer | Pass Criteria |
|------|------|-----------|
| location store start order | `integration-single-process` | the framework runtime starts automatic connection after registering the location store |
| remote query client | `integration-multi-process` | a location topology snapshot lookup succeeds |
| socket/Spot monitoring source name mismatch | `unit` | startup validation exception |
| missing location monitoring runtime | `unit` | startup validation exception if a location source is registered but there's no location runtime |
| registry polling diff | `integration-multi-process` | topology, status, service summary events occur |
| spot polling diff | `integration-multi-process` | status, peers, subjects events occur |

## 8. Release Gate

To ship a release, all five of the following must be satisfied.

1. `unit`, `integration-single-process`, `integration-multi-process` all pass
2. Both `net8.0` and `net10.0` pass
3. CI gate passes across all six runtime RIDs above
4. happy-path samples and representative failure-paths are each covered at least once
5. every disallowed combination defined in the responsible spec is pinned by a test

In other words, running the sample once alone is not enough. Startup validation and runtime failure
meaning must also be pinned by tests.

Also, even if the native backend already supports a given platform, the framework stacks
registration, lifecycle, DI, and monitoring layers on top of it. So the platform gate is kept
separate from the backend gate.

## 9. Explicit Turn Terminator Regression

| Test Case | Pass Criteria |
|---------------|-----------|
| `SerialExecutorTests.SerialExecutionQueue_DefaultAwait_Holds_Gate_Until_Work_Completes` | an ordinary callback doesn't start the next work item of the same execution line until it finishes. |
| `WorkerPoolTests.RunCpuWorker_Async_Holds_Serial_Turn_Until_Work_Completes` | while waiting on `Async(...)`, the Spot turn is kept so the next callback doesn't start. |
| `WorkerPoolTests.RunCpuWorker_Yield_Releases_And_Resumes_Through_Serial_Turn` | `Yield(...)` gives back the Spot turn and resumes the completion continuation through the same execution line's queue. |
| `SerialExecutorTests.SerialExecutionQueue_AutomaticTurn_Fault_Cleans_Pending_Turn` | after a `Yield(...)` target fails, the pending turn is cleaned up and the execution line can keep being used. |
| `SerialExecutorTests.SerialExecutionQueue_AutomaticTurn_Cancellation_Cleans_Pending_Turn` | after a `Yield(...)` target is canceled, the pending turn is cleaned up and the next work item runs. |
| `E2E:ATD-B3` | while waiting on an actor join's `Yield(...)`, another actor's request completes first, and the continuation returns to the original actor mailbox. |
| `E2E:ATD-A4` | while waiting on a worker's `Yield(...)`, the Spot turn is given back, and the continuation resumes on the original Spot execution line. |

## 10. Per-Document Regression Test Section

Every contract, sample, and internals document in this directory must have a short
`Regression Tests` section describing which tests pin its own items. Updating only the central matrix
isn't enough — a reader of the detailed document can easily miss which tests to look at otherwise.

| Test Case | Pass Criteria |
|---------------|-----------|
| `RegressionTests.DotNetContractDocuments_AllExposeRegressionTestSection` | all the documents below have a `Regression Tests` section. |
| `RegressionTests.DotNetRegressionMatrix_References_AllContractDocuments` | this matrix references all of the document filenames below. |

Whether each per-language exact-interface document's declaration still matches the source export and
package API snapshot is also confirmed by one common contract test. Naming an owner per document
keeps it from dropping out of the verification denominator when a document is added or renamed.

| Exact Interface Document | Regression Test |
|----------------------|-------------|
| `01-common-runtime.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |
| `02-configuration-host.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |
| `03-configuration-topology.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |
| `04-channel-messaging.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |
| `05-spots.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |
| `06-actors.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |
| `07-bound-stream-session.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |
| `07-stream-session.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |
| `08-authority-relocation.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |
| `08-location-maintenance.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |
| `08-location-provider-redis.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |
| `10-monitoring-errors.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |
| `10-topology-monitoring.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |
| `11-serialization.ko.md` | `ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports` |

The target documents are as follows.

- `README.ko.md`
- `01-system-structure.ko.md`
- `02-handler-interfaces.ko.md`
- `03-stream-connector.ko.md`
- `04-routing-id-allocation.ko.md`
- `05-route-mesh.ko.md`
- `06-location-store.ko.md`
- `dotnet-http-client.ko.md`
- `regression-test-matrix.ko.md`
- `../../common/internals/README.ko.md`
- `public-symbol-delta-v11.ko.md`
- `backend-dependency-policy.ko.md`

[^public-contract]: The public contract means the API surface exposed to external users, whose compatibility must be maintained on change.
[^regression]: A regression is a phenomenon where a feature that worked well in a previous version breaks again because of a new change. A regression test is a test bundle that always runs to prevent that.
[^topology]: Topology is the configuration information describing which nodes (channel, spot, registry, etc.) exist where, and how they're connected to each other.
[^rid]: An RID (Runtime Identifier) is the string `.NET` uses to identify an OS/CPU combination. E.g., `win-x64`, `linux-arm64`.
[^ci-gate]: A CI gate is the bundle of automated verification steps (build, test, etc.) that must pass before merging or deploying a new change.
[^di]: DI (Dependency Injection) is a pattern where an object doesn't build its own dependencies directly — they're injected by an external container. In `.NET`, an `IServiceCollection`/`IServiceProvider`-based container is the standard.
[^backpressure]: Backpressure is the mechanism that throttles the flow so the sender can't push messages faster than the receiver can process them.
[^hwm]: HWM (High Water Mark) is the maximum number of bytes a send queue can hold — backpressure kicks in once this limit is reached.
[^wire-multipart]: Wire multipart is a way of sending one logical message split into several message parts, such as header and payload. This lets routing work even by looking at just one part.
[^entry-spot]: The Entry Spot is the stateless entry Spot an Object Server process creates when it starts. It processes, per actor mailbox, Actors not yet belonging to a User Spot.
[^spot]: `SPOT` is a dynamically created and destroyed logical unit of execution. Room, stage, and zone are representative examples.
[^binding-token]: A session binding token is a token that identifies the connection state between an actor and a stream session, used to tell which binding is current on reconnect.

---
<!-- framework-adapter-nav:bottom:start -->
[Document List](../../../README.en.md) | [Previous: Runtime Execution](../../common/internals/02-serialization.en.md) | [Next: Backend Dependency Policy](backend-dependency-policy.en.md)
<!-- framework-adapter-nav:bottom:end -->

## 11. Regression Test Items Moved From The Public Contract Documents

When the per-language spec was compressed into 3 documents (system structure · interfaces ·
connector), the regression items owned by the deleted per-feature contract documents were moved into
this section. The contract's meaning is owned by the common spec.

### Channel

| Test Case | Pass Criteria |
|---------------|-----------|
| `ChannelsTests.AddZLinkFramework_Throws_WhenChannelNameIsDuplicated` | duplicate registration of the same channel name causes a startup validation exception. |
| `ChannelsTests.AddZLinkFramework_Throws_WhenClientHasNoPeerAcquisitionPath` | fails before starting if the client role has neither automatic connection (store) nor manual connection. |
| `E2E:RM-A2` | verifies a client request marker on the manual endpoint connection path. |
| `E2E:RM-C1` | client/server request and send are both processed between actual processes. |
| `E2E:RM-A1` | a store-automatic-connection-based client processes requests across actual multiple processes. |
| `ZLinkAsyncSubmitterTests.Async_DrainsPendingItemFromReadyCallback` | the async submitter drains pending items in the ready callback, without duplicate-sending. |

### SPOT

| Test Case | Pass Criteria |
|---------------|-----------|
| `NodesAndServicesTests.AddZLinkFramework_Throws_WhenSpotFactoryTypeIsDuplicatedAcrossNodes` | duplicate registration of the same Spot factory type causes a startup validation exception. |
| `NodesAndServicesTests.AddZLinkFramework_AllowsStandaloneLocalSpotNode` | the `SpotNode` in this test's current name refers to the internal Object runtime. Verifies that a local-only Object Server configuration with no Location Store starts. |
| `E2E:SM-A6` | Spot initialize and the explicit close lifecycle each complete once on an actual runtime. |
| `E2E:SM-E2` | a Spot timer tick is delivered to the registered handler. |
| `E2E:SM-E3` | an idle timer closes the Spot, and the subsequent request fails, jointly verifying timer and lifecycle termination. |
| `E2E:SM-E4` | the timer overrun policy limits late ticks according to the contract. |
| `CoverageCriticalRuntimeTests.SpotTimerFailureEventFactory_MapsStoppedAndContinuingFailures` | a handler exception is reflected in monitoring events, distinguished as a continuing failure vs. a timer-stopping failure. |
| `E2E:SM-C4` | an external Spot publisher client publishes to the target SPOT channel. |
| `E2E:SM-B7` | actor creation, join, and packet dispatch run in the current Spot's lifecycle order. |
| `EntrySpotActorDispatchTests.EntrySpotActorDispatch_ConcurrentActors_StartsOutsideEntrySpotSerialLine_AndKeepsSameActorOrdering` | an Entry Spot actor packet preserves the same actor's order, but a different actor's start isn't blocked by the Entry Spot serial execution line. The per-actor mailbox is also the ordering basis even at the same actor dispatch boundary as the native actor-readable path. |
| `E2E:ATD-C2` | the same timer's next tick runs only after the previous callback's continuation and completion, without re-entering. |

### Actor

| Test Case | Pass Criteria |
|---------------|-----------|
| --- | --- |
| `NodesAndServicesTests.AddZLinkFramework_Throws_WhenActorFactoryNameIsDuplicated` | a duplicate actor factory name is blocked with an exception at startup validation. |
| `E2E:SM-B7` | actor creation, user Spot join, and actor packet dispatch order continue on an actual node. |
| `EntrySpotActorDispatchTests.EntrySpotActorDispatch_ConcurrentActors_StartsOutsideEntrySpotSerialLine_AndKeepsSameActorOrdering` | an Entry Spot actor packet follows per-actor mailbox order, and a different actor's handler start isn't blocked by the Entry Spot serial execution line. |
| `E2E:SM-G2` | after the logical owner changes, an actor packet is processed only at the new owner, not dispatched to the previous Spot. |
| `E2E:SM-D2` | a request is delivered from a stream session to a remote bound actor, and the reply returns on the same session. |
| `E2E:SM-D1` | local actor bind and relay complete the request/reply on the same session. |
| `EntrySpotActorDispatchTests.EntrySpotActorDispatch_NoBindRequest_RepliesViaNoBind_AndDoesNotBindSession` | an Entry Spot actor request responds with the request handler's result and doesn't fall back to a send handler or session bind path. |
| `ScaffoldSmokeTests.PublicSurface_Removes_ActorReply_And_StreamClientContracts` | the actor context Reply and actor stream client contract are not re-exposed in the public API surface. |

### STREAM

| Test Case | Pass Criteria |
|---------------|-----------|
| `NodesAndServicesTests.AddZLinkFramework_Throws_WhenStreamNodeRegistersMultipleSessions` | duplicate session registration on the same node causes a startup validation exception. |
| `StreamSessionForcedCleanupTests.Stream_node_preserves_typed_routing_id_from_backend_callback` | the typed routing id received from the transport callback is delivered to session dispatch with no loss of information. |
| `E2E:SM-D7` | stream authentication and dispatch complete between an actual connector and session node. |
| `E2E:SM-D8` | a stream shutdown fails the pending request, and messaging resumes after the new session's authentication and bind. |

### Monitoring

| Test Case | Pass Criteria |
|---------------|-----------|
| `CoverageCriticalRuntimeTests.MonitoringEventMapper_MapsAndFiltersSocketEvents` | maps socket runtime events into public monitoring events, without leaking internal events outward. |
| `CoverageCriticalRuntimeTests.SpotTimerFailureEventFactory_MapsStoppedAndContinuingFailures` | a timer handler exception is turned into typed events, distinguished as a continuing failure vs. a timer-stopping failure. |
| `MonitoringTests.AddZLinkMonitoring_RequiresPositivePollingIntervals` | Spot and location runtime polling intervals must be greater than 0. |
| `MonitoringTests.MonitoringSourceValidator_RequiresLocationRuntimeForLocationSources` | rejects before starting if a location source is registered but there's no location runtime. |
| `MonitoringTests.AddZLinkMonitoring_Throws_WhenSpotSourceDoesNotMatchRegisteredSpotNode` | the `SpotNode` in this test's current name refers to the internal Object runtime. Rejects before starting if the Spot source name differs from the registered runtime. |
| `MonitoringTests.AddZLinkMonitoring_UsesExplicitSpotSourceWithoutAutoDiscovery` | a Spot source is not added automatically — only an explicitly specified Object runtime is registered. |
| `MonitoringTests.SpotPollingEventDiff_EmitsSealedVariantsOnlyForChangedSnapshotParts` | precisely emits the initial snapshot and subsequent diffs as sealed Spot event variants. |
| Common Concept | `.NET` Type / Member |
| Observation Level | `ZLinkDiagnosticsLevel` { `Off`, `Errors` (default), `Normal`, `Detailed` } |
| Internal Event | `ZLinkMessageFlowEvent` is a runtime-internal record, not a public callback argument. |
| Diagnostic Options | `IZLinkDispatchOptions.Diagnostics` → `IZLinkDiagnosticsOptions.SetLevel(...)`, `SetSampleRate(...)`, `IncludeMessageSizes(...)` |
| Runtime Toggle | `IZLinkDiagnosticsRuntime.Level` (DI singleton) |
| Standard Output | `ActivitySource("Zlink.Framework")`, `ILogger` structured log |
| Common Concept | `.NET` |
| Meter Name (Constant) | `ZLinkMeters.Framework` = `"zlink.framework"`, matching the [common metrics contract](../../common/spec/25-runtime-metrics.en.md) byte-exact |
| Instrument Emission | `System.Diagnostics.Metrics.Meter("zlink.framework")` — `Counter`/`UpDownCounter`/`ObservableGauge`/`Histogram` |
| App Wiring (Common Case) | OTel `MeterProviderBuilder.AddMeter(ZLinkMeters.Framework)` — that's all |
| Non-OTel/Test Collection | .NET's standard `MeterListener` subscribes to `ZLinkMeters.Framework` directly — no zlink-specific listener interface |
| Common Concept | `.NET` |
| Creation Gate | Flow correlation info is created only when the diagnostics level isn't `Off` and the current processing stage passes the level/sampling condition. |
| Internal Event Fields | `ZLinkMessageFlowEvent`'s `FlowId` and `FlowOrigin` are not public DTOs — they're internal inputs used to build standard trace/log attributes. |
| Common Concept | `.NET` |
| Automatic Shutdown | the framework's hosted service joins `IHostApplicationLifetime` shutdown and, in `StopAsync`, awaits the host `ShutdownAsync(...)` terminal result |
| Relocate Order | all-or-none preflight → admission seal → current turn completion → queue/journal/timer freeze → Actor/Spot relocation → STREAM barrier → authority commit |
| User Spot Aggregate | relocates a User Spot and its member Actors as one aggregate, verifying every participant and generation in the Location Store commit |
| Spot Creation Boundary | an Instance marker is specified on the direct Spot send/request's fluent builder. A missing Instance starts cold placement only when the marker selects exactly one factory type |
| Terminal Result | the enum numbers and allowed combinations of `ZLinkFrameworkRelocationResult` and `ZLinkFrameworkTerminationResult` match the [graceful drain contract](../../common/spec/28-graceful-drain-handoff.en.md) |
| Explicit Control | `FrameworkRuntimeContracts.Relocation_and_shutdown_are_separate_host_operations`: the DI singleton `IZLinkFrameworkRuntime` separates `RelocateAsync(options, ...)` and `ShutdownAsync(...)`. The relocation deadline lives in options, and `deadline == null` is 30 seconds for both operations |
| Concurrent Caller | after `Relocating`, the same relocation caller shares the shared operation's mode, deadline, and terminal result. `Blocked` is not put into the terminal cache |
| Readiness Probe | `IZLinkFrameworkRuntime.Status.IsReady` is true only when the host is `Serving`, and component readiness includes the host state projection |
| Transport Liveness | with no public option, RouteMesh/ClientServer apply a 5-second probe/ACK and a 15-second inbound deadline. Classic fanout applies a 5-second one-way beacon and a 15-second inbound deadline on each publisher's dedicated SUB socket, becoming ready after the first valid receive |

### Session Actor Dispatch

| Test Case | Pass Criteria |
|---------------|-----------|
| `E2E:SM-D2` | relays a remote actor request from a session callback and returns the reply on the same stream session. |
| `E2E:SM-D5` | verifies the difference between a client close and an explicit actor disconnect notification with actual lifecycle markers. |
| `E2E:SM-D1` | local actor binding and relay work without a separate remote route fallback. |
| `E2E:SM-D6` | a bound session push reaches only the specified client session. |
| `EntrySpotActorDispatchTests.EntrySpotActorDispatch_ConcurrentActors_StartsOutsideEntrySpotSerialLine_AndKeepsSameActorOrdering` | an Entry Spot actor packet preserves the same actor's order, and a different actor's handler start isn't blocked by the Entry Spot serial execution line. |
| `SerialExecutorTests.ActorDispatchMailbox_Runs_Waiters_In_Fifo_Order` | actor packets that didn't enter a user Spot also follow per-actor mailbox registration order. |
| `E2E:SM-G2` | after the actor owner changes, a request that was waiting is also processed at the new owner and not delivered to the previous location. |
| `E2E:SM-B7` | packet dispatch after actor join runs in the current Spot's lifecycle order. |
| `E2E:SM-D8` | after a previous stream ends, re-authenticates and re-binds on a new session and resumes messaging. |
| `StreamSessionForcedCleanupTests.Rejected_terminal_work_starts_disposal_and_releases_the_session_scope` | even if the session termination work is rejected from the queue, stream close and session scope cleanup still complete. |
| `SerialExecutorTests.StreamSessionSerialExecutor_Continues_After_Work_Exception` | a fire-and-forget work exception in the session queue is recorded in the error sink and doesn't block the next work item. |
| `SerialExecutorTests.SpotSerialExecutor_Continues_After_Queued_Work_Exception` | a fire-and-forget work exception in the Spot queue is recorded in the error sink and doesn't block the next work item. |
| `SerialExecutorTests.SpotSerialExecutor_ExecuteAsync_Propagates_Work_Exception` | the completion-waiting execution path in the Spot queue returns the handler exception straight to the caller. |
| `SerialExecutorTests.SerialExecutionQueue_RunAsync_Propagates_Work_Exception` | the common serial queue's `RunAsync(...)` records a work exception to the error sink while also propagating it to the caller. |
| `SerialExecutorTests.SerialExecutionQueue_Wait_Cancellation_Does_Not_Remove_Queued_Work` | even if a completion wait is canceled in the common serial queue, a work item already in the queue is not removed. |
| `SerialExecutorTests.ActorDispatchCancellation_Does_Not_Stop_Current_Or_Later_Dispatch` | canceling an actor dispatch wait doesn't stop the currently running dispatch or a later dispatch. |
| `RegressionTests.DotNetRegressionMatrix_Includes_ExecutionSerialization_Guards` | the central regression matrix keeps its execution-serialization regression items. |

### Entry Spot

| Test Case | Pass Criteria |
|---------------|-----------|
| `E2E:SM-A1` | a request using the Framework-issued Entry SpotId reaches the actual Entry Spot handler. |
| `ScaffoldSmokeTests.PublicSurface_Removes_DirectRouteContracts_And_Exposes_ActorContracts` | removed route contracts aren't re-exposed in the public API surface, and actor/session contracts are kept. |

### Stage Wrapper

| Test Case | Pass Criteria |
|---------------|-----------|
| `E2E:SM-B7` | after an actor join, packets in the stage-role Spot are processed in the right lifecycle order. |
| `E2E:SM-E3` | a timer used as the stage tick doesn't create further callbacks after the Spot shuts down. |
| `E2E:SM-A5` | the application stage wrapper runs Spot requests, timers, and lifecycle through the public API. |

### E2E Inventory And Aggregate Runner

| Test Case | Pass Criteria |
|---------------|-----------|
| `RegressionTests.EveryCommonE2EConfigHasAnExplicitAggregateRunnerEntry` | common E2E Configs 1 through 14 all have a `.NET` feature map, process runner, and aggregate runner entry. A configuration that isn't implemented isn't counted as a success by the runner. |
| `RegressionTests.CommonE2EConfigsHaveCompleteDotNetFeatureMapInventories` | Configs 1 through 14's common scenario IDs correspond to `.NET` feature-map IDs with no gaps, duplicates, or unknowns. |
| `E2E:RM-A2` | `LocationMessaging`'s actual process selector leaves client-visible results and role server evidence. |
| `SCRIPT:e2e/ChannelEgressRouting/run_e2e.sh` | Config 12's incomplete `all` selector closes with exit 2, printing the missing selector. |
| `SCRIPT:e2e/InstanceSpot/run_e2e.sh` | Config 14's missing process fixture closes with exit 2 and is not counted as an aggregate success. |

### Location

| Test Case | Pass Criteria |
|---------------|-----------|
| `NodesAndServicesTests.AddZLinkFramework_AddLocationStores_ResolvesEveryStoreRoleToOneInstance` | one store instance is registered for every location role. |
| `NodesAndServicesTests.AddZLinkFramework_Throws_WhenAddLocationStoreIsCombinedWithInMemoryStore` | fails before starting if an external store and an in-memory store are registered together. |
| `LocationResolverTests.Rows_Of_Expired_Owner_Are_Not_Returned` | the resolver doesn't return a location row whose owner lease has expired. |
| `AuthorityStoreTests.Missing_Version_Can_Be_Compared_And_Conflict_Returns_Current` | handles missing and found with the same opaque expected-version CAS. |
| `AuthorityStoreTests.Owner_And_Relocation_Phase_Change_In_One_Cas` | the Actor/Instance owner and relocation phase change together in one payload. |
| `RelocationStoreTests.Retention_Is_Exactly_24Hours` | the Framework passes exactly 24-hour retention based on the provider clock. |
| `RelocationStoreTests.Delete_Missing_Is_Idempotent` | reading a missing relocation payload is a closed result, and a repeated delete is a successful cleanup. |
| `FrameworkRuntimeTests.ApplicationVersion_Uses_Long_Numeric_Order` | verifies `0..long.MaxValue` ordering and rejection of a negative value at startup. |
| `FrameworkRuntimeTests.Factory_Policy_Hides_Relocation_Protocol` | the adapter receives only opaque application state, not authority/relocation-root/journal details. |
| `AutoConnectReconcilerTests.Reconcile_Connects_New_Targets_And_Disconnects_Vanished_Ones` | automatic connection connects new peers and removes vanished peers from the connection set. |
| `RedisInMemoryParityTests.Same_Operation_Sequence_Yields_Identical_Statuses_And_Generations` | the in-memory and Redis implementations return the same write status and generation. |
| `E2E:RM-A1`, `E2E:RM-A4`, `E2E:RM-B1`, `E2E:RM-B2` | verifies store-based automatic connection, failover, scale-out, and scale-in on actual processes. |
| `E2E:SF-B1`, `E2E:SF-B2`, `E2E:SF-C1`, `E2E:SF-C2`, `E2E:SF-D3` | verifies keeping connections during a store outage, recovery, owner lease expiry, and normal-shutdown cleanup. |
| `RegressionTests.DotNet_Samples_Do_Not_Use_Legacy_Registry_Discovery` | .NET samples don't use the removed Registry/Discovery API again |
