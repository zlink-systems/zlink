import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendSpotRouteBridge } from '../backend/contracts';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import { closeMessages, decodeChannelReply, type ZLinkChannelEnvelopeCodecRegistry } from './channel-envelope';
import { appendParts } from './channel-multipart';
import { awaitWithAbort } from '../abort';

export class ZLinkSpotRouteBridgeTransport {
  constructor(
    private readonly bridges: ReadonlyMap<string, ZLinkBackendSpotRouteBridge>,
    private readonly defaultRequestTimeoutMs: number | undefined
  ) {}

  has(routerChannelId: string): boolean {
    return this.bridges.has(routerChannelId);
  }

  async send(target: ZLinkSpotRouteTarget, parts: readonly Message[], signal?: AbortSignal): Promise<void> {
    const bridge = this.requireBridge(target.routerChannelId);
    try {
      await awaitWithAbort(appendParts(
        bridge.send(target.routerChannelId, target.targetNodeRid, target.spotId),
        parts
      ).submit(), signal);
    } finally {
      closeMessages(parts);
    }
  }

  async request<TReply>(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<TReply> {
    const bridge = this.requireBridge(target.routerChannelId);
    const operation = appendParts(
      bridge.request(target.routerChannelId, target.targetNodeRid, target.spotId),
      parts
    ).timeout(timeoutMs ?? 0).submit();
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

  requestRaw(
    target: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<readonly Message[]> {
    const bridge = this.requireBridge(target.routerChannelId);
    const operation = bridge.request(target.routerChannelId, target.targetNodeRid, target.spotId)
      .message(request)
      .timeout(timeoutMs ?? this.defaultRequestTimeoutMs ?? 0)
      .submit();
    return awaitWithAbort(operation, signal, () => {
      void operation.then(closeMessages, () => undefined);
    });
  }

  private requireBridge(routerChannelId: string): ZLinkBackendSpotRouteBridge {
    const bridge = this.bridges.get(routerChannelId);
    if (bridge === undefined) throw new ZLinkConfigurationException(`Route channel '${routerChannelId}' has no SPOT route bridge.`);
    return bridge;
  }
}
