export const SpotActorTransferNames = {
  mesh: 'spot-actor-transfer',
  actorTypeStateful: 'transfer-stateful',
  actorTypeEmptyState: 'transfer-empty-state',
  actorTypeNoAdapter: 'transfer-no-adapter',
  actorTypeFailTransferOut: 'transfer-fail-out',
  actorTypeFailLeave: 'transfer-fail-leave',
  actorTypeFailTransferIn: 'transfer-fail-in',
  packetJoin: 'JoinTargetReq',
  packetProbe: 'ProbeReq',
  packetHandoff: 'HandoffProbe',
  packetBoundPush: 'BoundPushReq',
  packetBoundNotify: 'BoundPushNotify',
  packetBindActor: 'BindActorSessionReq'
} as const;

export interface ActorCreateReq {
  actorId: string;
  actorType: string;
  stateVersion: number;
  applicationStateBytes?: number;
}
export interface ActorCreateRes {
  actorId: string;
  actorType: string;
  objectGeneration: string;
  meshName: string;
  nodeRid: string;
}
export interface CreateSpotReq { spotId: string; mode?: string }
export interface CreateSpotRes { spotId: string; nodeRid: string; state: string }
export interface GateReleaseRes { key: string; released: boolean }
export class JoinTargetReq {
  constructor(
    readonly scenario: string,
    readonly targetSpotId: string,
    readonly expectedMode?: string,
    readonly transferId?: string
  ) {}
}
export interface JoinTargetRes {
  scenario: string;
  actorId: string;
  accepted: boolean;
  sourceNodeRid: string;
  targetSpotId: string;
  stateVersion: number;
  errorKind?: string;
}
export class ProbeReq {
  constructor(
    readonly scenario: string,
    readonly marker: string,
    readonly delayMs?: number,
    readonly requestTimeoutMs?: number
  ) {}
}
export class HandoffProbe extends ProbeReq {}
export interface ProbeRes {
  scenario: string;
  actorId: string;
  spotId: string;
  nodeRid: string;
  stateVersion: number;
  marker: string;
}
export interface BindActorSessionReq {
  scenario: string;
  actorId: string;
  objectGeneration?: string;
  meshName?: string;
  nodeRid?: string;
  transferId?: string;
}
export interface BindActorSessionRes {
  scenario: string;
  actorId: string;
  objectGeneration: string;
  meshName: string;
  nodeRid: string;
}
export class BoundPushReq {
  constructor(readonly scenario: string, readonly marker: string) {}
}
export interface BoundPushRes extends ProbeRes {}
export class BoundPushNotify implements ProbeRes {
  constructor(
    readonly scenario: string,
    readonly actorId: string,
    readonly spotId: string,
    readonly nodeRid: string,
    readonly stateVersion: number,
    readonly marker: string
  ) {}
}
export interface EvidenceWaitReq { containsAll: readonly string[]; timeoutMilliseconds?: number }
export interface ActorRefRes {
  actorId: string;
  objectGeneration: string;
  meshName: string;
  nodeRid: string;
}
export interface TransferStateDto {
  actorId: string;
  actorType: string;
  stateVersion: number;
  applicationStateBytes?: number;
}
export interface ActorEvidence {
  scenario: string;
  actorId: string;
  kind: string;
  value: string;
  nodeRid: string;
  atNs: string;
  sequence: number;
  transferId?: string;
}
