# Node.js Location 운영 조회와 observability 공개 interface

이 문서는 application이 상태를 조회하고 event를 처리할 때 사용하는 공개 interface만 정의한다. 저장 행 변화 감시, runtime event 발행, serializer 선택과 handler 호출 wrapper는 Framework 내부 책임이다.

## 1. Handler filter

Filter는 message 정보와 공개 dispatch 종류만 포함하는 전용 context를 받는다. Socket, endpoint,
내부 owner 종류와 decoded message는 공개하지 않는다. `AbortSignal`은 dispatch가 취소될 때 전달된다.

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

`ChannelSend`와 `ChannelRequest`는 RouteMesh와 ClientServer Channel을 함께 나타낸다. RouteMesh와
Node direct는 MeshName을 제공한다. ClientServer와 classic fanout은 MeshName을 제공하지 않는다.

Filter는 `next()`를 최대 한 번 호출한다. 두 번째 호출은
`ZLinkFrameworkErrorKind.InvalidOperation`으로 실패하며 handler를 다시 실행하지 않는다. Request에서
`next()`를 호출하지 않으면 `ZLinkFrameworkErrorKind.Rejected` reply를 보낸다. Filter의 반환값으로
업무 reply를 만들거나 바꾸지 않는다.

`ZLinkHandlerInvocation`은 public contract가 아니다. Filter는 Node direct send/request, Channel
send/request와 classic fanout 구독 handler에만 적용한다. Spot·Actor·Logical Multicast·STREAM
handler에는 적용하지 않는다.

## 2. Location 운영 조회

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

Spot·Actor·route 저장 행 query, 저장 key, `ZLinkLocationAutoConnectType`, watch store와 change stamp는 runtime 내부 계약이다. Application은 aggregate topology와 service summary를 조회한다.

## 3. Runtime 상태와 structured log

Application은 이 문서의 runtime interface가 반환하는 immutable status와 변화 stream으로
현재 상태를 확인한다. Socket·Location raw event DTO, event handler, sink와 monitoring
source 등록 option은 public contract가 아니다.

상태가 바뀐 이유는 application이 구성한 표준 structured logger에 기록한다. Native socket
event, Location 저장 행 변화와 Spot timer 실패를 public callback으로 전달하지 않는다.

## 4. Host relocation과 termination runtime

Object relocation과 host 종료는 `ZLinkFrameworkRuntime`의 `relocate(options)`와 `shutdown()`으로 각각
시작한다.
RouteMesh topology runtime은 상태 조회만 제공하며 host lifecycle을 변경하지 않는다.

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

`relocate(options)`가 성공하면 runtime은 `Relocated` 상태가 되고 process와 infrastructure connection은 유지된다.
호출자는 결과가 `Relocated`인지 확인한 뒤 `shutdown()`을 호출할 수 있으며, relocation이 필요하지 않으면
`shutdown()`만 호출한다. `Relocating`에서 `shutdown()`을 호출하면 실행 중인 atomic relocation unit만
terminal 상태까지 확정하고 나머지 relocation을 중단한다. 이때 relocation waiter는
`Blocked/ShutdownRequested`를 받는다.
`signal`은 해당 Promise의 대기만 취소한다. 이미 시작된 shared relocation 또는 shutdown operation과
다른 waiter에는 영향을 주지 않는다.

호출자는 relocation mode를 생략할 수 없다. `PlannedMaintenance`는 같은 application version을 유지하는
node 점검이나 재부팅에 사용한다. 이 mode에서 `targetApplicationVersion`을 지정하면 Promise는 application
admission을 변경하기 전에 `TypeError`로 reject된다. 유효한 호출의
`effectiveTargetApplicationVersion`은 source host의 application version이다.

`RollingUpdate`는 `targetApplicationVersion`이 필수이며 source version보다 커야 한다. 값이 없거나 source
version 이하이면 같은 방식으로 `TypeError`로 reject된다. Framework는 지정한 version과 정확히 같은 node만
후보로 사용하고 중간 version이나 더 높은 다른 version으로 대체하지 않는다.

Target 후보는 다음 순서로 줄인다.

1. 같은 Mesh에서 `Serving` 상태인 Object Server를 찾는다.
2. Planned maintenance이면 source version, rolling update이면 지정한 target version과 정확히 같은
   node만 남긴다.
3. Source와 같은 maintenance wave에 속한 node를 제외한다.
4. stable type, relocation policy와 adapter capability가 맞는지 확인한다.
5. population capacity와 reservation 가능 여부를 확인한다.
6. 남은 후보에 node-wide placement weight를 적용한다.

Version filter를 capability·capacity·weight보다 먼저 적용하므로 다른 version으로 fallback하지 않는다.
조건을 만족하는 Ready target이 없으면 `Blocked/TargetUnavailable`이다.

같은 shared relocation이 실행 중일 때 mode와 effective target version이 같은 호출은 기존 operation에
참여하고 같은 terminal result를 받는다. 첫 호출의 `deadlineMs`가 shared operation deadline을 고정하며
뒤에 참여한 호출은 이를 변경하지 않는다. Mode 또는 target version이 다른 호출은 실행 중인 operation을
변경하거나 대기열에 넣지 않고 `Blocked/OperationInProgress`를 반환한다. 이 결과에는 거부된 호출이
요청한 mode와 effective target version을 기록한다.

## 5. RouteMesh runtime 상태와 readiness

`meshName`은 조회할 RouteMesh를 지정한다. 등록되지 않은 이름은 새 상태를 만들지 않고 typed route error로
실패한다. `isReady(...)`는 host가 `Serving`이고 해당 RouteMesh topology가 `Ready`일 때만 `true`다.

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

`placement.isAvailable`은 Actor 또는 Spot capacity와 activation concurrency에 모두
여유가 있을 때만 `true`다. Activation concurrency의 현재 값과 limit은 status에
별도 field로 노출하지 않는다.

`NotConnected`는 topology상 연결이 필요하지만 ready connection이 없는 상태다.
`NotRequired`는 두 Object Client 모두 RouteMesh Channel Server membership이 없어 연결이 필요하지 않은
정상 상태다. Channel Client membership만 등록한 경우도 같다. 어느 한쪽에라도 weight `0`을 포함한
Channel Server membership이 있으면 연결 부재는 `NotConnected`다. 두 상태 모두 ready peer 수에서
제외하지만 `NotRequired`는 liveness·health failure 집계에 포함하지 않는다.

## 6. ClientServer와 fanout runtime 상태

같은 process의 endpoint도 remote endpoint와 같은 후보 선택 및 연결 상태 계약을 따른다.
관찰 stream은 일부 field만 담은 event가 아니라 변경 뒤의 완전한 status를 전달한다.

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

Peer status는 `nodeRid`, `state`, `unavailableReason`만 제공한다. Lifecycle generation,
descriptor source, connection intent, admission·claim·drain 내부 상태와 pending request 수는
Framework가 연결과 ownership을 판정하는 데만 사용한다. Application은 이 값을 받지 않는다.

네 `observe(...)`가 전달하는 단위는 모두 `ZLinkObservedStatus<TStatus>`다. `status`는 변경 뒤의 완전한
status이며 관찰자 사이에 공유한다. `loss`는 이 async iteration 하나에만 해당하는 유실 누계이므로
status 안에 넣지 않는다. `coalescedCount`는 source별 최신 slot 합치기로 이 관찰자가 보지 못한 중간
status 수이고, `discardedTerminalCount`는 보관 상한 초과로 폐기한 terminal status 수다. 둘을 하나로
합치지 않는다 — 관찰자가 "따라잡기로 건너뛴 것"과 "영영 못 보는 것"을 구분해야 하기 때문이다. 두 값은
`sequence`와 같은 누적 값이므로 `bigint`이며 `observe(...)` 호출마다 `0n`에서 시작하고 같은 iteration
안에서 단조 증가한다. `9223372036854775807n`(`2^63 - 1`)을 넘으면 그 값으로 고정한다. Java `long`이 표현할 수 있는
최댓값이며, 네 언어가 같은 상한을 쓰도록 여기에 맞췄다.
Framework는 관찰자 queue가 가득 찼다는 이유로 iteration을 끝내지 않으며,
`signal` abort만 해당 iteration을 종료한다. 전달 단위의 정의는
[Runtime monitoring §3](../../../../24-runtime-monitoring.ko.md#3-현재-상태-조회와-변화-관찰)이 소유한다.

## 7. Message wrapper

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

Serializer registry 선택과 default serializer 결정 helper는 runtime 내부에 둔다. Application은 codec을 Framework 구성에 등록하고 message별 selector나 registry를 전달하지 않는다.
