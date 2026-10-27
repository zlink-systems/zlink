import { RequestRuntime } from '../foundation/request-runtime';
import type { PendingOperation } from '../foundation/operation-registry';
import { waitRequestReply } from '../messaging/request-deadline';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendSendFlags } from '../backend/contracts';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import {
  closeMessages,
  decodeChannelReply,
  type ZLinkChannelEnvelopeCodecRegistry
} from './channel-envelope';
import type { ZLinkChannelSocketRegistry } from './channel-socket-registry';

export class ZLinkRouterSocketSpotRouteTransport {
  constructor(
    private readonly sockets: ZLinkChannelSocketRegistry,
    private readonly requests = new RequestRuntime()
  ) {}

  async send(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    flags: ZLinkBackendSendFlags
  ): Promise<void> {
    const router = this.sockets.routeRouter(target.routerChannelId);
    try {
      await router.sendToSpot(target.targetNodeRid, target.spotId, parts, flags);
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
    signal?: AbortSignal,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>
  ): Promise<TReply> {
    return this.requests.request(
      timeoutMs ?? undefined,
      signal,
      async (requestOperation) => {
        const router = this.sockets.routeRouter(target.routerChannelId);
        void flags;
        const operation = router.requestToSpot(
          target.targetNodeRid,
          target.spotId,
          parts,
          timeoutMs,
          onAdmitted,
          requestOperation
        );
        const replyParts = await waitRequestReply(
          operation,
          'Spot request',
          requestOperation,
          undefined,
          closeMessages
        );
        try {
          return decodeChannelReply<TReply>(replyParts, codecs);
        } finally {
          closeMessages(replyParts);
          closeMessages(parts);
        }
      },
      existing
    );
  }

  async requestRaw(
    target: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>
  ): Promise<readonly Message[]> {
    return this.requests.request(
      timeoutMs ?? undefined,
      signal,
      async (requestOperation) => {
        const router = this.sockets.routeRouter(target.routerChannelId);
        const operation = router.requestToSpot(
          target.targetNodeRid,
          target.spotId,
          [request],
          timeoutMs,
          onAdmitted,
          requestOperation
        );
        return await waitRequestReply(
          operation,
          'Spot request',
          requestOperation,
          undefined,
          closeMessages
        );
      },
      existing,
      closeMessages
    );
  }
}
