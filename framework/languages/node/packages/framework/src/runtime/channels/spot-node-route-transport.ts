import type { Message } from '../../contracts/Common/Message';
import type { ZLinkFrameworkRegistration } from '../configuration';
import { awaitWithAbort, throwIfAborted } from '../abort';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import {
  closeMessages,
  decodeChannelReply,
  type ZLinkChannelEnvelopeCodecRegistry
} from './channel-envelope';
import { ZLinkSpotRouteTargetResolver } from './spot-route-target-resolver';

export class ZLinkSpotNodeRouteTransport {
  constructor(
    private readonly registration: ZLinkFrameworkRegistration,
    private readonly targets: ZLinkSpotRouteTargetResolver
  ) {}

  async send(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    signal?: AbortSignal
  ): Promise<boolean> {
    const router = this.targets.spotNodeRouter(target.routerChannelId);
    if (router === undefined) {
      return false;
    }
    try {
      throwIfAborted(signal);
      await awaitWithAbort(
        router.sendToSpot(target.targetNodeRid, target.spotId, parts),
        signal
      );
      return true;
    } finally {
      closeMessages(parts);
    }
  }

  request<TReply>(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<TReply> | undefined {
    const router = this.targets.spotNodeRouter(target.routerChannelId);
    if (router === undefined) {
      return undefined;
    }
    const effectiveTimeoutMs = timeoutMs ?? this.registration.requestTimeoutMs ?? 30_000;
    throwIfAborted(signal);
    const operation = router.requestToSpot(
        target.targetNodeRid,
        target.spotId,
        parts,
        effectiveTimeoutMs
      );
    return awaitWithAbort(operation, signal, () => {
      void operation.then(closeMessages, () => undefined);
    }).then((replyParts) => {
        try {
          return decodeChannelReply<TReply>(replyParts, codecs);
        } finally {
          closeMessages(replyParts);
          closeMessages(parts);
        }
      });
  }

  requestRaw(
    target: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<readonly Message[]> | undefined {
    const router = this.targets.spotNodeRouter(target.routerChannelId);
    if (router === undefined) {
      return undefined;
    }
    const effectiveTimeoutMs = timeoutMs ?? this.registration.requestTimeoutMs ?? 30_000;
    throwIfAborted(signal);
    const operation = router.requestToSpot(
      target.targetNodeRid,
      target.spotId,
      request,
      effectiveTimeoutMs
    );
    return awaitWithAbort(operation, signal, () => {
      void operation.then(closeMessages, () => undefined);
    });
  }
}
