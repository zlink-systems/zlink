import { RequestRuntime } from '../foundation/request-runtime';
import type { PendingOperation, OperationObserver } from '../foundation/operation-registry';
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

  request<TReply>(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
    flags: ZLinkBackendSendFlags,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>,
    destination?: OperationObserver<unknown, unknown>
  ): Promise<TReply> {
    try {
      const operation = this.requests.begin(timeoutMs, signal, existing);
      return this.requests.run<TReply>(
        operation,
        (receiver) => {
          const router = this.sockets.routeRouter(target.routerChannelId);
          void flags;
          router.requestToSpot(
            target.targetNodeRid,
            target.spotId,
            parts,
            timeoutMs,
            onAdmitted,
            operation,
            receiver
          );
        },
        (value) => {
          const replyParts = value as readonly Message[];
          try {
            return decodeChannelReply<TReply>(replyParts, codecs);
          } finally {
            closeMessages(replyParts);
            closeMessages(parts);
          }
        },
        (error) => error,
        destination
      );
    } catch (error) {
      return Promise.reject(error);
    }
  }
  async requestRaw(
    target: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>
  ): Promise<readonly Message[]> {
    const operation = this.requests.begin(timeoutMs, signal, existing);
    return this.requests.run<readonly Message[]>(
      operation,
      (receiver) => {
        const router = this.sockets.routeRouter(target.routerChannelId);
        router.requestToSpot(
          target.targetNodeRid,
          target.spotId,
          [request],
          timeoutMs,
          onAdmitted,
          operation,
          receiver
        );
      },
      (value) => {
        const replyParts = value as readonly Message[];
        if (!operation.isPending()) closeMessages(replyParts);
        return replyParts;
      },
      (error) => error
    );
  }
}
