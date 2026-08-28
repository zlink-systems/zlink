import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendSendFlags } from '../backend/contracts';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import { awaitWithAbort } from '../abort';
import {
  closeMessages,
  decodeChannelReply,
  type ZLinkChannelEnvelopeCodecRegistry
} from './channel-envelope';
import type { ZLinkChannelSocketRegistry } from './channel-socket-registry';

export class ZLinkRouterSocketSpotRouteTransport {
  constructor(private readonly sockets: ZLinkChannelSocketRegistry) {}

  async send(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    flags: ZLinkBackendSendFlags,
    signal?: AbortSignal
  ): Promise<void> {
    const router = this.sockets.routeRouter(target.routerChannelId);
    try {
      await awaitWithAbort(
        router.sendToSpot(target.targetNodeRid, target.spotId, parts, flags),
        signal
      );
    } finally {
      closeMessages(parts);
    }
  }

  async request<TReply>(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
    flags: ZLinkBackendSendFlags,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<TReply> {
    const router = this.sockets.routeRouter(target.routerChannelId);
    void flags;
    const operation = router.requestToSpot(
      target.targetNodeRid,
      target.spotId,
      parts,
      timeoutMs
    );
    const replyParts = await awaitWithAbort(operation, signal, () => {
      void operation.then(closeMessages, () => undefined);
    });
    try {
      return decodeChannelReply<TReply>(replyParts, codecs);
    } finally {
      closeMessages(replyParts);
      closeMessages(parts);
    }
  }

  async requestRaw(
    target: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<readonly Message[]> {
    const router = this.sockets.routeRouter(target.routerChannelId);
    const operation = router.requestToSpot(
      target.targetNodeRid,
      target.spotId,
      [request],
      timeoutMs
    );
    return await awaitWithAbort(operation, signal, () => {
      void operation.then(closeMessages, () => undefined);
    });
  }

}
