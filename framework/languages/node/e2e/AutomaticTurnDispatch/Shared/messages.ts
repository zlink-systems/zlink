export const AutomaticTurnDispatchNames = {
  controlChannel: 'await.control',
  delayChannel: 'await.delay',
  spotChannel: 'await.spot',
  perActorSpotType: 'AwaitProbeSpotPerActor',
  spotRouteChannel: 'await.spot.route',
  streamNode: 'await.stream',
  actorType: 'await.actor',
  actorIdMetadata: 'actor-id',
  spotIdMetadata: 'spot-rid',
  targetNodeRidMetadata: 'target-node-rid'
} as const;

export class DelayReq {
  constructor(
    readonly requestId: string,
    readonly delayMs: number,
    readonly marker: string
  ) {}
}

export interface DelayRes {
  readonly requestId: string;
  readonly marker: string;
  readonly nodeRid: string;
}

export interface ExternalDelayRes {
  readonly requestId: string;
  readonly marker: string;
}

export interface AwaitShutdownScenarioReq {
  readonly requestId: string;
  readonly spotId: string;
  readonly delayMs: number;
}

export interface AwaitShutdownRecoveryReq {
  readonly requestId: string;
  readonly spotId: string;
}

export interface AwaitScenarioRes {
  readonly operation: string;
  readonly spotId: string;
  readonly evidence: readonly string[];
}

export interface HoldMsg {
  readonly requestId: string;
  readonly delayMs: number;
}

export interface AwaitMsg {
  readonly requestId: string;
  readonly delayMs: number;
  readonly correlationId: string;
  readonly terminator?: 'async' | 'yield';
}

export interface AwaitReq {
  readonly requestId: string;
  readonly delayMs: number;
  readonly correlationId: string;
  readonly terminator?: 'async' | 'yield';
}

export interface CounterResetMsg {
  readonly requestId: string;
}

export interface CounterAwaitMsg {
  readonly requestId: string;
  readonly operationId: string;
  readonly delayMs: number;
  readonly terminator: 'async' | 'yield';
}

export interface CounterReadReq {
  readonly requestId: string;
}

export interface CounterReadRes {
  readonly requestId: string;
  readonly value: number;
}

export interface HttpAwaitMsg {
  readonly requestId: string;
  readonly delayMs: number;
  readonly terminator: 'async' | 'yield';
}

export interface IoWorkerBatchReq {
  readonly requestId: string;
  readonly count: number;
  readonly delayMs: number;
}

export interface IoWorkerBatchRes {
  readonly requestId: string;
  readonly completed: number;
}

export interface CpuWorkerAwaitMsg {
  readonly requestId: string;
  readonly delayMs: number;
  readonly terminator: 'async' | 'yield';
}

export interface SelfCycleMsg {
  readonly requestId: string;
  readonly timeoutMs: number;
  readonly terminator?: 'async' | 'yield';
}

export interface SelfSendMsg {
  readonly requestId: string;
  readonly marker: string;
}

export interface RemoteSpotAwaitReq {
  readonly requestId: string;
  readonly targetSpotId: string;
  readonly delayMs: number;
  readonly terminator?: 'async' | 'yield';
}

export interface RemoteSpotAwaitMsg {
  readonly requestId: string;
  readonly targetSpotId: string;
  readonly delayMs: number;
  readonly terminator?: 'async' | 'yield';
}

export interface WorkerAwaitMsg {
  readonly requestId: string;
  readonly delayMs: number;
}

export interface AwaitTimeoutMsg {
  readonly requestId: string;
  readonly delayMs: number;
  readonly timeoutMs: number;
  readonly terminator?: 'async' | 'yield';
}

export interface AwaitCancelMsg {
  readonly requestId: string;
  readonly delayMs: number;
  readonly cancelAfterMs: number;
  readonly terminator?: 'async' | 'yield';
}

export interface TimerStartMsg {
  readonly requestId: string;
  readonly timerName: string;
  readonly mode: string;
  readonly periodMs: number;
  readonly delayMs: number;
}

export interface TimerStopMsg {
  readonly requestId: string;
}

export interface ProbeMsg {
  readonly requestId: string;
  readonly marker: string;
}

export interface ProbeReq extends ProbeMsg {}

export interface EnsureSpotReq {
  readonly spotId: string;
  readonly executionMode?: 'spot_wide' | 'per_actor';
}

export interface EnsureSpotRes {
  readonly spotId: string;
  readonly nodeRid: string;
}

export interface AwaitEvidenceWaitReq {
  readonly requestId: string;
  readonly marker: string;
  readonly timeoutMilliseconds?: number;
}

export interface AwaitEvidenceReq {
  readonly requestId: string;
}

export interface AwaitEvidenceRes {
  readonly requestId: string;
  readonly evidence: readonly string[];
}

export interface BindAwaitActorsReq {
  readonly spotId: string;
  readonly actorIds: readonly string[];
}

export interface BindAwaitActorsRes {
  readonly spotId: string;
  readonly actors: readonly AwaitActorBinding[];
}

export interface AwaitActorBinding {
  readonly actorId: string;
  readonly nodeRid: string;
  readonly generation: string;
}

export interface ActorAwaitReq {
  readonly requestId: string;
  readonly delayMs: number;
  readonly terminator?: 'async' | 'yield';
}

export interface ActorFastReq {
  readonly requestId: string;
  readonly marker: string;
}

export interface ActorFastMsg {
  readonly requestId: string;
  readonly marker: string;
}

export interface ActorJoinAwaitReq {
  readonly requestId: string;
  readonly targetSpotId: string;
}

export interface DeferredJoinFailureMsg {
  readonly requestId: string;
  readonly firstActorId: string;
  readonly secondActorId: string;
  readonly firstTargetSpotId: string;
  readonly secondTargetSpotId: string;
  readonly failureMode: 'exception' | 'cancel';
}

export interface ActorPushAwaitReq {
  readonly requestId: string;
  readonly delayMs: number;
  readonly value: string;
}

export class ActorPushNotify {
  constructor(
    readonly actorId: string,
    readonly requestId: string,
    readonly value: string,
    readonly nodeRid: string
  ) {}
}

export interface ActorAwaitRes {
  readonly scenarioId: string;
  readonly requestId: string;
  readonly actorId: string;
  readonly spotId: string;
  readonly nodeRid: string;
  readonly marker: string;
}

export interface AutomaticTurnDispatchRes {
  readonly scenarioId: string;
  readonly requestId: string;
  readonly spotId: string;
  readonly nodeRid: string;
  readonly marker: string;
}

// Stream decoding produces plain objects. These nominal constructors restore the
// packet descriptor before a decoded packet is forwarded through framework calls.
export class AwaitShutdownScenarioReq {}
export class AwaitShutdownRecoveryReq {}
export class HoldMsg {}
export class AwaitMsg {}
export class AwaitReq {}
export class CounterResetMsg {}
export class CounterAwaitMsg {}
export class CounterReadReq {}
export class HttpAwaitMsg {}
export class IoWorkerBatchReq {}
export class CpuWorkerAwaitMsg {}
export class SelfCycleMsg {}
export class SelfSendMsg {}
export class RemoteSpotAwaitReq {}
export class RemoteSpotAwaitMsg {}
export class WorkerAwaitMsg {}
export class AwaitTimeoutMsg {}
export class AwaitCancelMsg {}
export class TimerStartMsg {}
export class TimerStopMsg {}
export class ProbeMsg {}
export class ProbeReq {}
export class EnsureSpotReq {}
export class AwaitEvidenceWaitReq {}
export class AwaitEvidenceReq {}
export class BindAwaitActorsReq {}
export class ActorAwaitReq {}
export class ActorFastReq {}
export class ActorFastMsg {}
export class ActorJoinAwaitReq {}
export class DeferredJoinFailureMsg {}
export class ActorPushAwaitReq {}
