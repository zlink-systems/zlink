import { ZLinkPacket } from '@zlink-systems/framework';

export const PacketNames = {
  actorNotify: 'ActorNotify',
  actorAsk: 'ActorAsk',
  actorPush: 'ActorPush',
  bindActor: 'BindActor'
} as const;

export class ActorNotify {
  constructor(
    readonly scenario: string,
    readonly actorId: string,
    readonly value: string
  ) {}
}

export class ActorAsk {
  constructor(
    readonly scenario: string,
    readonly actorId: string,
    readonly value: string
  ) {}
}

export interface ActorReply {
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

@ZLinkPacket(PacketNames.actorPush)
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

export interface ActorEnsureResponse {
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

export interface SessionBindingSnapshot {
  readonly actorId: string;
  readonly sessionIds: readonly string[];
}

export interface ActorCallRequest {
  readonly scenario: string;
  readonly actorId: string;
  readonly actor?: ActorRefPayload;
  readonly value: string;
}

export interface ActorCallResponse {
  readonly scenario: string;
  readonly actorId: string;
  readonly result: string;
  readonly errorKind?: string;
}

export function actorNotify(scenario: string, actorId: string, value: string): ActorNotify {
  return new ActorNotify(scenario, actorId, value);
}

export function actorAsk(scenario: string, actorId: string, value: string): ActorAsk {
  return new ActorAsk(scenario, actorId, value);
}

export function actorPush(scenario: string, actorId: string, value: string): ActorPushReq {
  return new ActorPushReq(scenario, actorId, value);
}
