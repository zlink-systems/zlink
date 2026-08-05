import type { ActorRef, ZLinkMessage } from '../Common';
import type { ZLinkActor } from '../Actors';

export interface ZLinkSessionActors {
  readonly bound: readonly ZLinkSessionActor[];
  bind(actor: ZLinkActor | ActorRef, signal?: AbortSignal): Promise<ZLinkSessionActor>;
  bindOrGet(actor: ActorRef, signal?: AbortSignal): Promise<ZLinkSessionActor>;
  find(actorId: string): ZLinkSessionActor | undefined;
}

export interface ZLinkSessionActor {
  readonly actorId: string;
  readonly ref: ActorRef;
  relay(payload: ZLinkMessage, signal?: AbortSignal): Promise<void>;
  notifyDisconnected(signal?: AbortSignal): Promise<void>;
}
