import type { ActorRef, ZLinkMessage } from '../Common';
import type { ZLinkSessionDispatchContext } from './IZLinkSession';

export interface ZLinkSessionActors {
  readonly bound: readonly ZLinkSessionActor[];
  bind(actor: ActorRef, signal?: AbortSignal): Promise<ZLinkSessionActor>;
  bindOrGet(actor: ActorRef, signal?: AbortSignal): Promise<ZLinkSessionActor>;
  find(actorId: string): ZLinkSessionActor | undefined;
}

export interface ZLinkSessionActor {
  readonly actorId: string;
  readonly ref: ActorRef;
  relay(payload: ZLinkMessage, signal?: AbortSignal): Promise<void>;
  relay(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage, signal?: AbortSignal): Promise<void>;
  notifyDisconnected(signal?: AbortSignal): Promise<void>;
}
