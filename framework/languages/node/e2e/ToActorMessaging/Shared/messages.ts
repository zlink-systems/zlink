import { ZLinkPacket } from '@zlink-systems/framework';

export const PacketNames = {
  actorMsg: 'ActorMsg',
  actorReq: 'ActorReq',
  actorPushReq: 'ActorPushReq',
  actorPushNotify: 'ActorPushNotify',
  bindActorReq: 'BindActorReq'
} as const;

export class ActorMsg {
  constructor(
    readonly scenario: string,
    readonly actorId: string,
    readonly value: string
  ) {}
}

export class ActorReq {
  constructor(
    readonly scenario: string,
    readonly actorId: string,
    readonly value: string
  ) {}
}

export interface ActorRes {
  readonly scenario: string;
  readonly actorId: string;
  readonly value: string;
}

export class ActorPushReq {
  constructor(
    readonly scenario: string,
    readonly actorId: string,
    readonly value: string
  ) {}
}

@ZLinkPacket(PacketNames.actorPushNotify)
export class ActorPushNotify {
  constructor(
    readonly scenario: string,
    readonly actorId: string,
    readonly value: string
  ) {}
}

export interface ActorEvidence {
  readonly scenario: string;
  readonly actorId: string;
  readonly kind: string;
  readonly value: string;
}

export interface ActorRefPayload {
  readonly nodeRid: string;
  readonly actorId: string;
  readonly objectGeneration: string;
  readonly meshName: string;
}

export interface ActorEnsureRes {
  readonly actorId: string;
  readonly actor: ActorRefPayload;
}

export interface BindActorReq {
  readonly actor: ActorRefPayload;
}

export interface BindActorRes {
  readonly actorId: string;
  readonly nodeRid: string;
  readonly objectGeneration: string;
  readonly boundCount: number;
}

export interface SessionBindingRes {
  readonly actorId: string;
  readonly sessionIds: readonly string[];
}

export interface ActorCallReq {
  readonly scenario: string;
  readonly actorId: string;
  readonly actor?: ActorRefPayload;
  readonly value: string;
}

export interface ActorCallRes {
  readonly scenario: string;
  readonly actorId: string;
  readonly result: string;
  readonly errorKind?: string;
}

export function actorMsg(scenario: string, actorId: string, value: string): ActorMsg {
  return new ActorMsg(scenario, actorId, value);
}

export function actorReq(scenario: string, actorId: string, value: string): ActorReq {
  return new ActorReq(scenario, actorId, value);
}

export function actorPush(scenario: string, actorId: string, value: string): ActorPushReq {
  return new ActorPushReq(scenario, actorId, value);
}
