import type { RoutingId, ZLinkMessage } from '../Common';

export interface ZLinkStream {
  readonly sessionId: string;
  readonly routingId?: RoutingId;
  readonly localAddr?: string;
  readonly remoteAddr?: string;
  write(payload: ZLinkMessage, flags?: number): boolean;
  close(signal?: AbortSignal): Promise<void>;
}
