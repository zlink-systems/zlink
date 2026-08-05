import type { ActorId, ActorRef } from '../Common';
import type { SpotRef } from '../Spots';

export interface ZLinkActorManager {
  create(actorId: ActorId, actorType: string): ZLinkActorCreateCall;
  getOrCreate(actorId: ActorId, actorType: string): ZLinkActorGetOrCreateCall;
  find(actorId: ActorId, signal?: AbortSignal): Promise<ActorRef | undefined>;
  findSpot(actorId: ActorId, signal?: AbortSignal): Promise<SpotRef | undefined>;
  destroy(actor: ActorRef, signal?: AbortSignal): Promise<boolean>;
}

export interface ZLinkActorCreateCall {
  inMesh(meshName: string): this;
  request(request: unknown): this;
  timeout(timeoutMs: number): this;
  submit(signal?: AbortSignal): Promise<ZLinkActorCreateResult>;
  yield(signal?: AbortSignal): Promise<ZLinkActorCreateResult>;
}

export interface ZLinkActorGetOrCreateCall {
  inMesh(meshName: string): this;
  request(request: unknown): this;
  timeout(timeoutMs: number): this;
  submit(signal?: AbortSignal): Promise<ZLinkActorCreateResult>;
  yield(signal?: AbortSignal): Promise<ZLinkActorCreateResult>;
}

export type ZLinkActorCreateResult =
  | { readonly status: 'existing'; readonly actor: ActorRef }
  | { readonly status: 'created'; readonly actor: ActorRef; readonly reply?: unknown }
  | { readonly status: 'rejected'; readonly reply?: unknown };
