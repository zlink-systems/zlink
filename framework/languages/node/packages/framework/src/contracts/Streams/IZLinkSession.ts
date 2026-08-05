import type { RoutingId, ZLinkMessage } from '../Common';
import type { ZLinkSessionActors } from './IZLinkSessionActor';
import type { ZLinkStreamError } from './ZLinkStreamError';
import type { ZLinkSessionHandlerRegistry } from './IZLinkSessionPacketHandler';

export interface ZLinkSession {
  readonly context: ZLinkSessionContext;
  onConnected?(context: ZLinkSessionContext): Promise<void>;
  onDisconnected?(context: ZLinkSessionContext): Promise<void>;
  onError?(context: ZLinkSessionContext, error: ZLinkStreamError): Promise<void>;
  onDispatch?(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void>;
}

export interface ZLinkSessionDispatchContext {
  readonly packetName: string;
  readonly metadata: ReadonlyMap<string, string>;
  readonly canReply: boolean;
}


export interface ZLinkSessionFactory<TSession extends ZLinkSession = ZLinkSession> {
  create(context: ZLinkSessionContext): Promise<TSession>;
}

export interface ZLinkSessionContext {
  readonly sessionId: string;
  readonly routingId?: RoutingId;
  readonly localAddr?: string;
  readonly remoteAddr?: string;
  readonly client: ZLinkSessionClient;
  readonly actors: ZLinkSessionActors;
  readonly handlers: ZLinkSessionHandlerRegistry;
  close(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkSessionClient {
  send(message: unknown): ZLinkSessionSendCall;
  reply(message: unknown): ZLinkSessionReplyCall;
}

export interface ZLinkSessionSendCall {
  metadata(key: string, value: string): this;
  compress(enabled?: boolean): this;
  submit(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkSessionReplyCall {
  compress(enabled?: boolean): this;
  submit(signal?: AbortSignal): Promise<void>;
}
