export const SpotServiceNames = {
  spotChannel: 'spot.service',
  controlChannel: 'spot.control',
  externalSpotChannel: 'spot.external.play-a',
  externalSpotChannelB: 'spot.external.play-b',
  externalClientChannel: 'spot.external.client',
  spotEventTopic: 'spot.service.events',
  streamNode: 'session-stream',
  tlsStreamNode: 'session-stream-tls',
  playSpotNode: 'play-node',
  multiSpotNodeA: 'multi-node-a',
  multiSpotNodeB: 'multi-node-b',
  spotOnlyMesh: 'spot-only.mesh',
  multiRouteChannelA: 'multi-route-a',
  multiRouteChannelB: 'multi-route-b',
  actorType: 'scenario-player',
  alternateActorType: 'scenario-player-alternate',
  actorIdMetadata: 'actor-id'
} as const;

export interface CreateSpotReq {
  readonly spotId: string;
}

export interface CreateSpotRes {
  readonly spotId: string;
  readonly nodeRid?: string;
  readonly objectGeneration?: string;
  readonly meshName?: string;
  readonly state: string;
}

export interface ScaleOutReadinessReq {
  readonly nodeRid: string;
  readonly timeoutMilliseconds?: number;
}

export interface ScaleOutReadinessRes {
  readonly nodeRid: string;
  readonly peerReady: boolean;
  readonly entrySpotReady: boolean;
  readonly capabilities: readonly string[];
}

export interface ScaleOutActorProbeReq {
  readonly actorId: string;
  readonly marker: string;
}

export interface ScaleOutActorProbeRes {
  readonly actorId: string;
  readonly nodeRid: string;
  readonly marker: string;
}

export interface CloseSpotReq {
  readonly spotId: string;
}

export interface CloseSpotRes {
  readonly spotId: string;
  readonly closed: boolean;
}

export interface CloseSpotExactReq {
  readonly spotId: string;
  readonly objectGeneration: string;
  readonly meshName: string;
  readonly nodeRid: string;
}

export interface CloseSpotExactRes {
  readonly spotId: string;
  readonly closed: boolean;
  readonly staleGeneration: boolean;
  readonly errorKind?: string;
}

export interface StateReq {
  readonly operation: string;
  readonly delta: number;
}

export interface StateRes {
  readonly spotId: string;
  readonly nodeRid: string;
  readonly value: number;
}

export interface MultiNodeCreateSpotReq {
  readonly spotId: string;
  readonly delta: number;
}

export interface MultiNodeCreateSpotRes {
  readonly spotId: string;
  readonly nodeRid: string;
  readonly state: string;
  readonly value: number;
}

export interface MultiNodeStateRouteReq {
  readonly spotId: string;
  readonly delta: number;
}

export interface StateMsg {
  readonly marker: string;
}

export interface SpotOnlyMeshReq {
  readonly sourceSpotId: string;
  readonly targetSpotId: string;
  readonly marker: string;
}

export interface SpotOnlyMeshRes {
  readonly sourceSpotId: string;
  readonly targetSpotId: string;
  readonly targetValue: number;
  readonly marker: string;
}

export class SpotOnlyJoinReq {
  constructor(
    readonly targetSpotId: string,
    readonly actorId: string,
    readonly marker: string
  ) {}
}

export interface SpotOnlyJoinRes {
  readonly targetSpotId: string;
  readonly actorId: string;
  readonly accepted: boolean;
  readonly marker: string;
}

export interface StageProbeReq {
  readonly marker: string;
  readonly delta: number;
}

export interface StageTimerStartMsg {
  readonly name: string;
  readonly periodMs: number;
}

export interface SpotMsg {
  readonly marker: string;
}

export interface SpotOutboundMsg {
  readonly marker: string;
}

export interface SpotOutboundNegativeMsg {
  readonly marker: string;
}

export interface SpotOutboundRouteReq {
  readonly spotId: string;
  readonly marker: string;
}

export interface SpotOutboundRouteRes {
  readonly spotId: string;
  readonly marker: string;
  readonly accepted: boolean;
  readonly evidence: readonly string[];
}

export interface SpotToSpotReq {
  readonly targetSpotId: string;
  readonly marker: string;
}

export interface SpotToSpotRes {
  readonly sourceSpotId: string;
  readonly targetSpotId: string;
  readonly targetValue: number;
}

export interface SpotToSpotRouteReq {
  readonly sourceSpotId: string;
  readonly targetSpotId: string;
  readonly marker: string;
}

export interface SpotToSpotTimeoutReq {
  readonly targetSpotId: string;
  readonly marker: string;
}

export interface SpotToSpotTimeoutRes {
  readonly sourceSpotId: string;
  readonly targetSpotId: string;
  readonly failed: boolean;
}

export interface SpotToSpotTimeoutRouteReq {
  readonly sourceSpotId: string;
  readonly targetSpotId: string;
  readonly marker: string;
}

export interface SpotToSpotNegativeReq {
  readonly targetSpotId: string;
  readonly marker: string;
}

export interface SpotToSpotNegativeRes {
  readonly sourceSpotId: string;
  readonly targetSpotId: string;
  readonly requestFailed: boolean;
}

export interface SpotToSpotNegativeRouteReq {
  readonly sourceSpotId: string;
  readonly targetSpotId: string;
  readonly marker: string;
}

export interface SpotPublishReq {
  readonly spotId: string;
  readonly marker: string;
}

export interface SpotPublishRes {
  readonly operation: string;
  readonly publisherRid: string;
  readonly spotId: string;
  readonly marker: string;
  readonly evidence: readonly string[];
}

export interface SpotPublishObserveRes {
  readonly operation: string;
  readonly spotId: string;
  readonly marker: string;
  readonly received: boolean;
  readonly evidence: readonly string[];
}

export interface ChannelEchoReq {
  readonly value: string;
}

export interface ChannelEchoRes {
  readonly value: string;
}

export interface ChannelNotify {
  readonly marker: string;
}

export interface ChannelRouteReq {
  readonly value: string;
}

export interface ChannelRouteRes {
  readonly value: string;
}

export interface NodeRouteReq {
  readonly nodeRid: string;
  readonly value: string;
}

export interface NodeRouteRes {
  readonly value: string;
}

export interface SpotMixedRouteReq {
  readonly spotId: string;
  readonly nodeRid?: string;
  readonly channelValue: string;
  readonly nodeValue?: string;
  readonly delta: number;
}

export interface SpotMixedRouteRes {
  readonly spotId: string;
  readonly channelReply: string;
  readonly nodeReply?: string;
  readonly spotValue: number;
}

export interface ControlPingReq {
  readonly value: string;
}

export interface ControlPingRes {
  readonly value: string;
  readonly nodeRid: string;
}

export interface AuthReq {
  readonly actorId: string;
  readonly displayName: string;
  readonly nodeRid: string;
  readonly meshName?: string;
}

export interface AuthRes {
  readonly actorId: string;
  readonly nodeRid: string;
  readonly generation?: string;
}

export interface EnsureActorReq {
  readonly actorId: string;
  readonly displayName: string;
  readonly nodeRid: string;
  readonly meshName?: string;
}

export interface EnsureActorRes {
  readonly actorId: string;
  readonly nodeRid: string;
  readonly generation: string;
}

export interface ActorPingReq {
  readonly value: string;
}

export interface SlowActorPingReq {
  readonly value: string;
  readonly delayMs: number;
}

export interface ActorPingRes {
  readonly actorId: string;
  readonly nodeRid: string;
  readonly spotId: string;
  readonly value: string;
  readonly seen: number;
}

export class ActorPushReq {
  constructor(readonly value: string) {}
}

export class ActorPushNotify {
  constructor(
    readonly actorId: string,
    readonly value: string,
    readonly seen: number
  ) {}
}

export interface CrossRoleActorPushReq {
  readonly actorId: string;
  readonly nodeRid: string;
  readonly generation: string;
  readonly value: string;
}

export interface CrossRoleActorPushRes {
  readonly actorId: string;
  readonly nodeRid: string;
  readonly value: string;
  readonly delivered: boolean;
}

export interface MultiBindReq {
  readonly firstActorId: string;
  readonly secondActorId: string;
  readonly nodeRid: string;
}

export interface MultiBindRes {
  readonly boundCount: number;
}

export interface LogicalDisconnectReq {
  readonly actorId: string;
}

export interface LogicalDisconnectRes {
  readonly actorId: string;
  readonly remainingActorIds: readonly string[];
}

export interface SnapshotReq {
  readonly actorId: string;
}

export interface SnapshotRes {
  readonly actorId: string;
  readonly seen: number;
}

export interface DestroyActorReq {
  readonly actorId: string;
}

export interface DestroyActorRes {
  readonly actorId: string;
  readonly destroyed: boolean;
}

export interface UserSpotAuthReq {
  readonly spotId: string;
  readonly actorId: string;
  readonly displayName: string;
  readonly nodeRid: string;
}

export interface JoinUserSpotActorReq {
  readonly spotId: string;
  readonly actorId: string;
}

export interface JoinUserSpotActorRes {
  readonly spotId: string;
  readonly actorId: string;
  readonly accepted: boolean;
  readonly generation: string;
}

export interface LeaveReq {
  readonly actorId: string;
}

export interface LeaveRes {
  readonly actorId: string;
  readonly accepted: boolean;
}

export interface ComplexActorReq {
  readonly displayName: string;
  readonly level: number;
  readonly tags: readonly string[];
  readonly attributes: Readonly<Record<string, string>>;
}

export interface ComplexActorRes {
  readonly actorId: string;
  readonly displayName: string;
  readonly level: number;
  readonly tags: readonly string[];
  readonly attributes: Readonly<Record<string, string>>;
}

export interface SpotStateRouteReq extends StateReq {
  readonly spotId: string;
}

export interface SpotStateMsgReq {
  readonly spotId: string;
  readonly marker: string;
}

export interface SpotStateMsgRes {
  readonly spotId: string;
  readonly marker: string;
  readonly accepted: boolean;
  readonly evidence: readonly string[];
}

export interface SpotStageProbeReq extends StageProbeReq {
  readonly spotId: string;
}

export interface SpotStageTimerReq {
  readonly spotId: string;
  readonly name: string;
  readonly periodMs: number;
}

export interface SpotStageTimerRes {
  readonly spotId: string;
  readonly name: string;
  readonly started: boolean;
  readonly evidence: readonly string[];
}

export interface SpotMissingHandlerReq {
  readonly spotId: string;
}

export interface SpotMissingHandlerRes {
  readonly spotId: string;
  readonly failed: boolean;
  readonly evidence: readonly string[];
}

export interface SpotMissingMsgReq {
  readonly spotId: string;
  readonly marker: string;
}

export interface SpotMissingMsgRes {
  readonly spotId: string;
  readonly marker: string;
  readonly sent: boolean;
  readonly evidence: readonly string[];
}

export interface SpotMissingTargetReq {
  readonly spotId: string;
}

export interface SpotMissingTargetRes {
  readonly spotId: string;
  readonly failed: boolean;
  readonly errorKind?: string;
  readonly evidence: readonly string[];
}

export interface SpotMissingTargetMsgReq {
  readonly spotId: string;
  readonly marker: string;
}

export interface SpotMissingTargetMsgRes {
  readonly spotId: string;
  readonly marker: string;
  readonly sent: boolean;
  readonly failed?: boolean;
  readonly errorKind?: string;
  readonly evidence: readonly string[];
}

export interface SlowSpotReq {
  readonly marker: string;
  readonly delayMs: number;
}

export interface SlowSpotRes {
  readonly spotId: string;
  readonly nodeRid: string;
  readonly marker: string;
}

export interface SpotSlowRouteReq {
  readonly spotId: string;
  readonly marker: string;
  readonly delayMs: number;
  readonly timeoutMs: number;
}

export interface SpotSlowRouteRes {
  readonly spotId: string;
  readonly marker: string;
  readonly timedOut: boolean;
}

export interface SpotWorkerStartReq {
  readonly spotId: string;
  readonly marker: string;
  readonly delayMs: number;
}

export interface WorkerStartRes {
  readonly spotId: string;
  readonly nodeRid: string;
  readonly marker: string;
}

export interface SpotWorkerCompleteReq {
  readonly spotId: string;
  readonly marker: string;
}

export class SpotAdminReq {
  constructor(
    readonly operation: 'publish' | 'worker' | 'idleTimer' | 'timer' | 'overrunTimer',
    readonly marker?: string,
    readonly name?: string,
    readonly periodMs?: number,
    readonly delayMs?: number,
    readonly policy?: 'SkipLateTicks' | 'CatchUpBounded' | 'DelayNextTick'
  ) {}
}

export interface SpotAdminRes {
  readonly spotId: string;
  readonly nodeRid: string;
  readonly marker?: string;
}

export interface SpotWorkerCompleteRes {
  readonly spotId: string;
  readonly marker: string;
  readonly completed: boolean;
  readonly evidence: readonly string[];
}

export interface SpotTimerStartReq {
  readonly spotId: string;
  readonly name: string;
  readonly periodMs: number;
}

export interface SpotTimerStartRes {
  readonly spotId: string;
  readonly name: string;
  readonly started: boolean;
  readonly evidence: readonly string[];
}

export interface SpotIdleCloseReq {
  readonly spotId: string;
  readonly name: string;
  readonly periodMs: number;
}

export interface SpotIdleCloseRes {
  readonly spotId: string;
  readonly name: string;
  readonly closed: boolean;
  readonly evidence: readonly string[];
}

export interface SpotOverrunStartReq {
  readonly spotId: string;
  readonly name: string;
  readonly policy: string;
  readonly periodMs: number;
}

export interface SpotOverrunStartRes {
  readonly spotId: string;
  readonly name: string;
  readonly policy: string;
  readonly started: boolean;
  readonly evidence: readonly string[];
}

export interface SpotTypeMismatchReq {
  readonly spotId: string;
}

export interface SpotTypeMismatchRes {
  readonly spotId: string;
  readonly failed: boolean;
  readonly errorKind: string;
  readonly state: string;
}

export interface EvidenceWaitReq {
  readonly containsAll: readonly string[];
  readonly timeoutMilliseconds?: number;
}

export type SpotServicePacketType<T extends object> = new () => T;

export function spotServicePacket<T extends object>(type: SpotServicePacketType<T>, value: T): T {
  return Object.assign(new type(), value);
}

export class CreateSpotReq {}
export class StateReq {}
export class StateMsg {}
export class StageProbeReq {}
export class StageTimerStartMsg {}
export class SpotMsg {}
export class SpotOutboundMsg {}
export class SpotOutboundNegativeMsg {}
export class SpotToSpotReq {}
export class SpotToSpotTimeoutReq {}
export class SpotToSpotNegativeReq {}
export class SlowSpotReq {}
export class ChannelEchoReq {}
export class ChannelNotify {}
export class CrossRoleActorPushReq {}
export class ControlPingReq {}
export class EnsureActorReq {}
export class ScaleOutActorProbeReq {}
export class MissingSpotReq {
  declare readonly operation: string;
  declare readonly delta: number;
}
export class MissingSpotMsg {
  declare readonly marker: string;
}
export class MissingChannelReq {
  declare readonly value: string;
}
export class MissingChannelNotify {
  declare readonly marker: string;
}
