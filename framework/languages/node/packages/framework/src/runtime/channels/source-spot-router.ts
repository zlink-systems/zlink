import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendSpot } from '../backend/contracts';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import { awaitWithAbort, throwIfAborted } from '../abort';
import { closeMessages, ZLinkChannelMessageKind } from './channel-envelope';
import { decodeSpotDirectReply, encodeSpotDirectEnvelope } from './spot-direct-envelope';

export class ZLinkSourceSpotRouter {
  constructor(private readonly defaultRequestTimeoutMs: number | undefined) {}

  async request<TReply>(
    sourceSpot: ZLinkBackendSpot,
    target: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<TReply> {
    throwIfAborted(signal);
    const parts = [encodeSpotDirectEnvelope(
      ZLinkChannelMessageKind.Request,
      target.routerChannelId,
      packetName,
      request
    )] as readonly Message[];
    const operation = sourceSpot.requestToSpot(
        target.targetNodeRid,
        target.spotId,
        parts,
        timeoutMs ?? this.defaultRequestTimeoutMs
      );
    const replyParts = await awaitWithAbort(operation, signal, () => {
      void operation.then(closeMessages, () => undefined);
    });
    try {
      return decodeSpotDirectReply<TReply>(replyParts);
    } finally {
      closeMessages(replyParts);
      closeMessages(parts);
    }
  }

  async send(
    sourceSpot: ZLinkBackendSpot,
    target: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    timeoutMs?: number
  ): Promise<void> {
    throwIfAborted(signal);
    const parts = [encodeSpotDirectEnvelope(
      ZLinkChannelMessageKind.Command,
      target.routerChannelId,
      packetName,
      message,
      metadata
    )] as readonly Message[];
    try {
      void timeoutMs;
      await awaitWithAbort(
        sourceSpot.sendToSpot(target.targetNodeRid, target.spotId, parts),
        signal
      );
    } finally {
      closeMessages(parts);
    }
  }

  async requestRaw(
    sourceSpot: ZLinkBackendSpot,
    target: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<readonly Message[]> {
    throwIfAborted(signal);
    const operation = sourceSpot.requestToSpot(
        target.targetNodeRid,
        target.spotId,
        request,
        timeoutMs ?? this.defaultRequestTimeoutMs
    );
    return await awaitWithAbort(operation, signal, () => {
      void operation.then(closeMessages, () => undefined);
    });
  }
}
