import { RequestRuntime } from '../foundation/request-runtime';
import type { PendingOperation, OperationObserver } from '../foundation/operation-registry';
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

  request<TReply>(
    sourceSpot: ZLinkBackendSpot,
    target: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>,
    destination?: OperationObserver<unknown, unknown>
  ): Promise<TReply> {
    let parts: readonly Message[];
    const operation = this.requests.begin(
      timeoutMs ?? this.defaultRequestTimeoutMs,
      signal,
      existing
    );
    return this.requests.run<TReply>(
      operation,
      (receiver) => {
        throwIfAborted(signal);
        parts = [
          encodeSpotDirectEnvelope(
            ZLinkChannelMessageKind.Request,
            target.routerChannelId,
            packetName,
            request
          )
        ] as readonly Message[];
        sourceSpot.requestToSpot(
          target.targetNodeRid,
          target.spotId,
          parts,
          timeoutMs ?? this.defaultRequestTimeoutMs,
          onAdmitted,
          operation,
          receiver
        );
      },
      (value) => {
        const replyParts = value as readonly Message[];
        try {
          return decodeSpotDirectReply<TReply>(replyParts);
        } finally {
          closeMessages(replyParts);
          closeMessages(parts);
        }
      },
      (error) => error,
      destination
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
    const operation = this.requests.begin(
      timeoutMs ?? this.defaultRequestTimeoutMs,
      signal,
      existing
    );
    return this.requests.run<readonly Message[]>(
      operation,
      (receiver) => {
        throwIfAborted(signal);
        sourceSpot.requestToSpot(
          target.targetNodeRid,
          target.spotId,
          request,
          timeoutMs ?? this.defaultRequestTimeoutMs,
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
