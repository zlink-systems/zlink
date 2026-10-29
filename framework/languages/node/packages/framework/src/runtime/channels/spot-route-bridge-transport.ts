import { RequestRuntime } from '../foundation/request-runtime';
import type { PendingOperation, OperationObserver } from '../foundation/operation-registry';
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

  request<TReply>(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>,
    destination?: OperationObserver<unknown, unknown>
  ): Promise<TReply> {
    try {
      const operation = this.requests.begin(
        timeoutMs ?? this.defaultRequestTimeoutMs,
        signal,
        existing
      );
      return this.requests.run<TReply>(
        operation,
        (receiver) => {
          const bridge = this.requireBridge(target.routerChannelId);
          appendParts(
            bridge.request(target.routerChannelId, target.targetNodeRid, target.spotId),
            parts
          )
            .timeout(timeoutMs ?? this.defaultRequestTimeoutMs ?? 0)
            .submit(onAdmitted, operation, receiver);
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
  requestRaw(
    target: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>
  ): Promise<readonly Message[]> {
    const operation = this.requests.begin(
      timeoutMs ?? this.defaultRequestTimeoutMs,
      signal,
      existing
    );
    return this.requests.run<readonly Message[]>(
      operation,
      (receiver) => {
        const bridge = this.requireBridge(target.routerChannelId);
        appendParts(bridge.request(target.routerChannelId, target.targetNodeRid, target.spotId), [
          request
        ])
          .timeout(timeoutMs ?? this.defaultRequestTimeoutMs ?? 0)
          .submit(onAdmitted, operation, receiver);
      },
      (value) => {
        const replyParts = value as readonly Message[];
        if (!operation.isPending()) closeMessages(replyParts);
        return replyParts;
      },
      (error) => error
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
