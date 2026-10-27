import { RequestRuntime } from '../foundation/request-runtime';
import type { PendingOperation } from '../foundation/operation-registry';
import { waitRequestReply } from '../messaging/request-deadline';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendSpotRouteBridge } from '../backend/contracts';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import {
  closeMessages,
  decodeChannelReply,
  type ZLinkChannelEnvelopeCodecRegistry
} from './channel-envelope';
import { appendParts } from './channel-multipart';

export class ZLinkSpotRouteBridgeTransport {
  constructor(
    private readonly bridges: ReadonlyMap<string, ZLinkBackendSpotRouteBridge>,
    private readonly defaultRequestTimeoutMs: number | undefined,
    private readonly requests = new RequestRuntime()
  ) {}

  has(routerChannelId: string): boolean {
    return this.bridges.has(routerChannelId);
  }

  async send(target: ZLinkSpotRouteTarget, parts: readonly Message[]): Promise<void> {
    const bridge = this.requireBridge(target.routerChannelId);
    try {
      await appendParts(
        bridge.send(target.routerChannelId, target.targetNodeRid, target.spotId),
        parts
      ).submit();
    } finally {
      closeMessages(parts);
    }
  }

  async request<TReply>(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>
  ): Promise<TReply> {
    return this.requests.request(
      timeoutMs ?? this.defaultRequestTimeoutMs,
      signal,
      async (requestOperation) => {
        const bridge = this.requireBridge(target.routerChannelId);
        const operation = appendParts(
          bridge.request(target.routerChannelId, target.targetNodeRid, target.spotId),
          parts
        )
          .timeout(timeoutMs ?? 0)
          .submit(onAdmitted, requestOperation);
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

  requestRaw(
    target: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>
  ): Promise<readonly Message[]> {
    return this.requests.request(
      timeoutMs ?? this.defaultRequestTimeoutMs,
      signal,
      async (requestOperation) => {
        const bridge = this.requireBridge(target.routerChannelId);
        const operation = bridge
          .request(target.routerChannelId, target.targetNodeRid, target.spotId)
          .message(request)
          .timeout(timeoutMs ?? this.defaultRequestTimeoutMs ?? 0)
          .submit(onAdmitted, requestOperation);
        return waitRequestReply(
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

  private requireBridge(routerChannelId: string): ZLinkBackendSpotRouteBridge {
    const bridge = this.bridges.get(routerChannelId);
    if (bridge === undefined)
      throw new ZLinkConfigurationException(
        `Route channel '${routerChannelId}' has no SPOT route bridge.`
      );
    return bridge;
  }
}
