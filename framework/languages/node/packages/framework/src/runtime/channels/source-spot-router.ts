import { RequestRuntime } from '../foundation/request-runtime';
import type { PendingOperation } from '../foundation/operation-registry';
import { waitRequestReply } from '../messaging/request-deadline';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendSpot } from '../backend/contracts';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import { throwIfAborted } from '../abort';
import { closeMessages, ZLinkChannelMessageKind } from './channel-envelope';
import { decodeSpotDirectReply, encodeSpotDirectEnvelope } from './spot-direct-envelope';

export class ZLinkSourceSpotRouter {
  constructor(
    private readonly defaultRequestTimeoutMs: number | undefined,
    private readonly requests = new RequestRuntime()
  ) {}

  async request<TReply>(
    sourceSpot: ZLinkBackendSpot,
    target: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>
  ): Promise<TReply> {
    return this.requests.request(
      timeoutMs ?? this.defaultRequestTimeoutMs,
      signal,
      async (requestOperation) => {
        throwIfAborted(signal);
        const parts = [
          encodeSpotDirectEnvelope(
            ZLinkChannelMessageKind.Request,
            target.routerChannelId,
            packetName,
            request
          )
        ] as readonly Message[];
        const operation = sourceSpot.requestToSpot(
          target.targetNodeRid,
          target.spotId,
          parts,
          timeoutMs ?? this.defaultRequestTimeoutMs,
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
          return decodeSpotDirectReply<TReply>(replyParts);
        } finally {
          closeMessages(replyParts);
          closeMessages(parts);
        }
      },
      existing
    );
  }

  async send(
    sourceSpot: ZLinkBackendSpot,
    target: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    message: unknown,
    metadata?: ReadonlyMap<string, string>
  ): Promise<void> {
    const parts = [
      encodeSpotDirectEnvelope(
        ZLinkChannelMessageKind.Command,
        target.routerChannelId,
        packetName,
        message,
        metadata
      )
    ] as readonly Message[];
    try {
      await sourceSpot.sendToSpot(target.targetNodeRid, target.spotId, parts);
    } finally {
      closeMessages(parts);
    }
  }

  async requestRaw(
    sourceSpot: ZLinkBackendSpot,
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
        throwIfAborted(signal);
        const operation = sourceSpot.requestToSpot(
          target.targetNodeRid,
          target.spotId,
          request,
          timeoutMs ?? this.defaultRequestTimeoutMs,
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
