# Node.js Location Operational Query And Observability Public Interface

This document only defines the public interface an application uses to
query state and process events. Storage row change monitoring, runtime
event publishing, serializer selection, and the handler call wrapper are
framework-internal responsibilities.

## 1. Handler Filter

A filter receives a dedicated context containing only message
information and the public dispatch kind. Socket, endpoint, internal
owner kind, and the decoded message aren't exposed. `AbortSignal` is
delivered when the dispatch is cancelled.

```ts
export interface ZLinkMessageContext {
  readonly meshName?: string;
  readonly channelName?: string;
  readonly packetName: string;
  readonly contentType?: string;
  readonly metadata: ZLinkMessageMetadata;
  readonly correlationId?: string;
}

export enum ZLinkHandlerDispatchKind {
  NodeDirectSend = 'nodeDirectSend',
  NodeDirectRequest = 'nodeDirectRequest',
  ChannelSend = 'channelSend',
  ChannelRequest = 'channelRequest',
  ClassicFanout = 'classicFanout'
}

export interface ZLinkHandlerFilterContext extends ZLinkMessageContext {
  readonly dispatchKind: ZLinkHandlerDispatchKind;
}

export type ZLinkHandlerFilterNext = () => Promise<void>;

export interface ZLinkHandlerFilter {
  invoke(
    context: ZLinkHandlerFilterContext,
    next: ZLinkHandlerFilterNext,
    signal?: AbortSignal
  ): Promise<void>;
}
```

`ChannelSend` and `ChannelRequest` together represent RouteMesh and
ClientServer Channel. RouteMesh and Node direct provide MeshName.
ClientServer and classic fanout don't provide MeshName.

A filter calls `next()` at most once. A second call fails with
`ZLinkFrameworkErrorKind.InvalidOperation` and doesn't re-run the
handler. If `next()` isn't called on a request, a
`ZLinkFrameworkErrorKind.Rejected` reply is sent. A filter's return
value doesn't create or change the business reply.

`ZLinkHandlerInvocation` isn't the public contract. A filter only
applies to Node direct send/request, Channel send/request, and classic
fanout subscription handlers. It doesn't apply to Spot/Actor/Logical
Multicast/STREAM handlers.

## 2. Location Operational Query

```ts
export interface ZLinkLocationRuntimeQuery {
  getStatus(signal?: AbortSignal): Promise<ZLinkLocationRuntimeStatus>;
  listTopology(
    filter: ZLinkLocationTopologyFilter,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkLocationTopologyEntry>>;
  listServiceSummaries(
    filter: ZLinkLocationServiceSummaryFilter,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkLocationServiceSummary>>;
}

export interface ZLinkLocationTopologyFilter {
  readonly meshName?: string;
  readonly nodeRid?: RoutingId;
  readonly state?: ZLinkLocationTopologyState;
}

export interface ZLinkLocationTopologyEntry {
  readonly meshName: string;
  readonly nodeRid: RoutingId;
  readonly endpoint: string;
  readonly draining: boolean;
  readonly state: ZLinkLocationTopologyState;
  readonly updatedAt: Date;
}

export interface ZLinkLocationServiceSummaryFilter {
  readonly meshName?: string;
}

export interface ZLinkLocationServiceSummary {
  readonly meshName: string;
  readonly totalCount: number;
  readonly readyCount: number;
  readonly errorCount: number;
  readonly stoppedCount: number;
  readonly lastUpdatedAt: Date;
}

export interface ZLinkLocationReadiness {
  isPeerReady(
    meshName: string,
    role: ZLinkLocationRole,
    nodeRid?: RoutingId,
    signal?: AbortSignal
  ): Promise<boolean>;
}
```

Spot/Actor/route storage row queries, storage keys,
`ZLinkLocationAutoConnectType`, watch store, and change stamp are
runtime-internal contracts. The application queries aggregate topology
and service summary.

## 3. Runtime Status And Structured Log

The application confirms current state using the immutable status and
change stream this document's runtime interfaces return. Raw
socket/Location event DTOs, event handlers, sinks, and monitoring
source registration options aren't the public contract.

The reason state changed is recorded by the standard structured logger
the application configured. Native socket events, Location storage row
changes, and Spot timer failures aren't delivered as public callbacks.

## 4. Host Relocation And Termination Runtime

Object relocation and host termination each start with
`ZLinkFrameworkRuntime`'s `relocate(options)` and `shutdown()`. The
RouteMesh topology runtime only provides status queries and doesn't
change host lifecycle.

```ts
export enum ZLinkFrameworkRuntimeState {
  Preparing = 0,
  Serving = 1,
  Relocating = 2,
  Relocated = 3,
  Draining = 4,
  Stopped = 5,
  Error = 6
}

export enum ZLinkFrameworkRelocationOutcome {
  Relocated = 0,
  Blocked = 1
}

export enum ZLinkFrameworkRelocationMode {
  PlannedMaintenance = 0,
  RollingUpdate = 1
}

export enum ZLinkFrameworkRelocationReason {
  None = 0,
  TargetUnavailable = 1,
  StoreUnavailable = 2,
  RelocationDisabled = 3,
  StateIncompatible = 4,
  DeadlineExceeded = 5,
  RelocationFailed = 6,
  RuntimeNotReady = 7,
  ManualTopologyUnsupported = 8,
  ShutdownRequested = 9,
  OperationInProgress = 10
}

export interface ZLinkFrameworkRelocationOptions {
  readonly mode: ZLinkFrameworkRelocationMode;
  readonly targetApplicationVersion?: bigint;
  readonly deadlineMs?: number;
  readonly signal?: AbortSignal;
}

export interface ZLinkFrameworkRelocationResult {
  readonly mode: ZLinkFrameworkRelocationMode;
  readonly effectiveTargetApplicationVersion: bigint;
  readonly outcome: ZLinkFrameworkRelocationOutcome;
  readonly reason: ZLinkFrameworkRelocationReason;
}

export enum ZLinkFrameworkTerminationOutcome {
  Stopped = 0,
  ForceStopped = 1
}

export enum ZLinkFrameworkTerminationReason {
  None = 0,
  DeadlineExceeded = 1,
  TeardownFailed = 2
}

export interface ZLinkFrameworkTerminationResult {
  readonly outcome: ZLinkFrameworkTerminationOutcome;
  readonly reason: ZLinkFrameworkTerminationReason;
}

export interface ZLinkFrameworkLifecycleOptions {
  readonly deadlineMs?: number;
  readonly signal?: AbortSignal;
}

export interface ZLinkFrameworkRuntimeStatus {
  readonly state: ZLinkFrameworkRuntimeState;
  readonly isReady: boolean;
  readonly acceptingWork: boolean;
  readonly deadline?: Date;
  readonly relocationResult?: ZLinkFrameworkRelocationResult;
  readonly terminationResult?: ZLinkFrameworkTerminationResult;
  readonly inboundDispatch: ZLinkInboundDispatchStatus;
  readonly sequence: bigint;
  readonly observedAt: Date;
}

export interface ZLinkObservationLoss {
  readonly coalescedCount: bigint;
  readonly discardedTerminalCount: bigint;
}

export interface ZLinkObservedStatus<TStatus> {
  readonly status: TStatus;
  readonly loss: ZLinkObservationLoss;
}

export interface ZLinkInboundDispatchStatus {
  readonly applicationHwmBytes: bigint;
  readonly pendingPayloadBytes: bigint;
  readonly queuedPayloadBytes: bigint;
  readonly activePayloadBytes: bigint;
  readonly applicationReceivePaused: boolean;
  readonly pendingCompletionSends: bigint;
  readonly completionSendLimit: bigint;
}

export interface ZLinkFrameworkRuntime {
  readonly status: ZLinkFrameworkRuntimeStatus;
  observe(signal?: AbortSignal): AsyncIterable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>>;
  relocate(options: ZLinkFrameworkRelocationOptions): Promise<ZLinkFrameworkRelocationResult>;
  shutdown(options?: ZLinkFrameworkLifecycleOptions): Promise<ZLinkFrameworkTerminationResult>;
}
```

If `relocate(options)` succeeds, the runtime becomes `Relocated` state,
and process and infrastructure connections are kept. The caller can
confirm the result is `Relocated` and then call `shutdown()`, or, if
relocation isn't needed, only call `shutdown()`. Calling `shutdown()`
during `Relocating` only confirms the currently running atomic
relocation unit to a terminal state and aborts the rest of relocation.
At this point, the relocation waiter receives
`Blocked/ShutdownRequested`. `signal` only cancels that Promise's wait.
It doesn't affect an already-started shared relocation or shutdown
operation, or other waiters.

The caller can't omit the relocation mode. `PlannedMaintenance` is used
for a node check or reboot that keeps the same application version. If
`targetApplicationVersion` is specified in this mode, the Promise
rejects with `TypeError` before changing application admission. A
valid call's `effectiveTargetApplicationVersion` is the source host's
application version.

`RollingUpdate` requires `targetApplicationVersion`, which must be
greater than source version. If the value is missing or at most source
version, it's rejected with `TypeError` the same way. The framework
only uses a node exactly matching the specified version as a candidate,
and doesn't substitute an intermediate version or a different, higher
version.

Target candidates are narrowed in the following order.

1. Finds an Object Server in `Serving` state on the same Mesh.
2. Keeps only a node exactly matching the source version for planned
   maintenance, or the specified target version for rolling update.
3. Excludes a node belonging to the same maintenance wave as source.
4. Confirms stable type, relocation policy, and adapter capability
   match.
5. Confirms population capacity and reservation availability.
6. Applies node-wide placement weight to the remaining candidates.

Since the version filter is applied before capability/capacity/weight,
it doesn't fall back to a different version. If there's no Ready target
satisfying the condition, it's `Blocked/TargetUnavailable`.

While the same shared relocation is running, a call with the same mode
and effective target version joins the existing operation and receives
the same terminal result. The first call's `deadlineMs` fixes the shared
operation deadline, and a later joining call doesn't change it. A call
whose mode or target version differs doesn't change the running
operation or queue — it returns `Blocked/OperationInProgress`. This
result records the rejected call's requested mode and effective target
version.

## 5. RouteMesh Runtime Status And Readiness

`meshName` specifies the RouteMesh to query. An unregistered name fails
with a typed route error instead of creating new state. `isReady(...)`
is only `true` when the host is `Serving` and that RouteMesh topology
is `Ready`.

```ts
export enum ZLinkTopologyState {
  Starting = 0,
  Ready = 1,
  Degraded = 2,
  Stopping = 3,
  Stopped = 4,
  Failed = 5
}

export enum ZLinkPeerState {
  Connecting = 0,
  Ready = 1,
  Draining = 2,
  NotConnected = 3,
  NotRequired = 4
}

export enum ZLinkTopologyReason {
  RuntimeNotReady = 0,
  NoReadyPeer = 1,
  NoReadyTarget = 2,
  LocationUnavailable = 3,
  CapacityExceeded = 4,
  Draining = 5,
  InternalFailure = 6
}

export interface ZLinkPeerStatus {
  readonly nodeRid: RoutingId;
  readonly state: ZLinkPeerState;
  readonly unavailableReason?: ZLinkTopologyReason;
}

export interface ZLinkChannelStatus {
  readonly channelName: string;
  readonly isReady: boolean;
  readonly readyTargetCount: number;
}

export interface ZLinkPlacementStatus {
  readonly isAvailable: boolean;
  readonly activeActorCount: number;
  readonly activeSpotCount: number;
  readonly unavailableReason?: ZLinkTopologyReason;
}

export interface ZLinkRouteMeshStatus {
  readonly meshName: string;
  readonly state: ZLinkTopologyState;
  readonly isReady: boolean;
  readonly readyPeerCount: number;
  readonly channels: readonly ZLinkChannelStatus[];
  readonly peers: readonly ZLinkPeerStatus[];
  readonly placement: ZLinkPlacementStatus;
  readonly sequence: bigint;
  readonly observedAt: Date;
}

export interface ZLinkRouteMeshRuntime {
  snapshot(meshName: string): ZLinkRouteMeshStatus;
  observe(
    meshName: string,
    capacity?: number,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkRouteMeshStatus>>;
  isReady(meshName: string): boolean;
}
```

`placement.isAvailable` is only `true` when both Actor/Spot capacity
and activation concurrency have room. Activation concurrency's current
value and limit aren't exposed as a separate field in status.

`NotConnected` is a state where the topology needs a connection but
there's no ready connection. `NotRequired` is a normal state where
neither Object Client has RouteMesh Channel Server membership so a
connection isn't needed. The same applies when only Channel Client
membership is registered. If either side has Channel Server membership,
including weight `0`, absence of connection is `NotConnected`. Both
states are excluded from ready peer count, but `NotRequired` isn't
included in liveness/health failure aggregation.

## 6. ClientServer And Fanout Runtime Status

An endpoint on the same process also follows the same candidate
selection and connection status contract as a remote endpoint. The
observation stream delivers a complete status after a change, not an
event holding only some fields.

```ts
export type ZLinkClientServerRole = 'client' | 'server' | 'clientAndServer';
export interface ZLinkClientServerTargetStatus {
  readonly nodeRid: RoutingId;
  readonly weight: number;
  readonly state: ZLinkPeerState;
  readonly unavailableReason?: ZLinkTopologyReason;
}

export interface ZLinkClientServerStatus {
  readonly channelName: string;
  readonly localRole: ZLinkClientServerRole;
  readonly state: ZLinkTopologyState;
  readonly isReady: boolean;
  readonly readyTargetCount: number;
  readonly targets: readonly ZLinkClientServerTargetStatus[];
  readonly sequence: bigint;
  readonly observedAt: Date;
}

export interface ZLinkClientServerRuntime {
  snapshot(channelName: string): ZLinkClientServerStatus;
  observe(
    channelName: string,
    capacity?: number,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkClientServerStatus>>;
  isReady(channelName: string): boolean;
}

export interface ZLinkFanoutStatus {
  readonly channelName: string;
  readonly state: ZLinkTopologyState;
  readonly isReady: boolean;
  readonly readyPublisherCount: number;
  readonly publishers: readonly ZLinkPeerStatus[];
  readonly sequence: bigint;
  readonly observedAt: Date;
}

export interface ZLinkFanoutRuntime {
  snapshot(channelName: string): ZLinkFanoutStatus;
  observe(
    channelName: string,
    capacity?: number,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkFanoutStatus>>;
}
```

Peer status only provides `nodeRid`, `state`, `unavailableReason`.
Lifecycle generation, descriptor source, connection intent, internal
admission/claim/drain state, and pending request count are only used by
the framework to judge connection and ownership. The application
doesn't receive these values.

The unit all four `observe(...)` deliver is `ZLinkObservedStatus<TStatus>`.
`status` is the complete status after a change, shared across
observers. `loss` is a loss accumulator specific to this one async
iteration, so it isn't put inside status. `coalescedCount` is the
number of intermediate statuses this observer didn't see because of
per-source latest-slot coalescing, and `discardedTerminalCount` is the
number of terminal statuses discarded for exceeding the retention cap.
The two aren't merged into one — because an observer must distinguish
"skipped by catching up" from "never seen at all." Since both values
are a cumulative value like `sequence`, they're `bigint`, starting at
`0n` per `observe(...)` call and increasing monotonically within the
same iteration. Once they exceed `9223372036854775807n` (`2^63 - 1`),
they're pinned at that value. This is the maximum value a Java `long`
can represent, and it's matched here so all four languages use the
same cap. The framework doesn't end the iteration just because the
observer's queue is full — only a `signal` abort ends that iteration.
The definition of the delivery unit is owned by
[Runtime Monitoring §3](../../../../24-runtime-monitoring.en.md#3-querying-current-state-and-observing-changes).

## 7. Message Wrapper

```ts
export declare class ZLinkMessage<TValue = unknown> {
  private constructor();
  static from<T>(value: T): ZLinkMessage<T>;
  static fromEncoded(payload: ZLinkEncodedPayload): ZLinkMessage;
  decode<T>(type?: Type<T>): T;
  toEncodedPayload(): ZLinkEncodedPayload;
  isEncoded(): boolean;
}

```

Serializer registry selection and the default-serializer decision
helper are kept internal to the runtime. The application registers a
codec in the Framework configuration, and doesn't pass a per-message
selector or registry.
