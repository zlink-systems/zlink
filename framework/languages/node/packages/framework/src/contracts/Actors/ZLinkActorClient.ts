import type { ActorId } from '../Common';

export interface ZLinkActorClient {
  sendToActor(actorId: ActorId, message: unknown): ZLinkActorSendCall;
  requestToActor(actorId: ActorId, request: unknown): ZLinkActorRequestCall;
}

export interface ZLinkActorSendCall {
  metadata(key: string, value: string): this;
  submit(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkActorRequestCall {
  metadata(key: string, value: string): this;
  timeout(timeoutMs: number): this;
  submit<TReply>(signal?: AbortSignal): Promise<TReply>;
  yield<TReply>(signal?: AbortSignal): Promise<TReply>;
}
