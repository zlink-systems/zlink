import { RequestRuntime } from '../foundation/request-runtime';
import type { PendingOperation } from '../foundation/operation-registry';
import { waitRequestReply } from '../messaging/request-deadline';
import { DEFAULT_REQUEST_TIMEOUT_MS } from '../../contracts/Configuration/Registration';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkFrameworkRegistration } from '../configuration';
import { throwIfAborted } from '../abort';
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
    private readonly targets: ZLinkSpotRouteTargetResolver,
    private readonly requests = new RequestRuntime()
  ) {}

  async send(target: ZLinkSpotRouteTarget, parts: readonly Message[]): Promise<boolean> {
    const router = this.targets.spotNodeRouter(target.routerChannelId);
    if (router === undefined) {
      return false;
    }
    try {
      await router.sendToSpot(target.targetNodeRid, target.spotId, parts);
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
    signal?: AbortSignal,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>
  ): Promise<TReply> | undefined {
    const router = this.targets.spotNodeRouter(target.routerChannelId);
    if (router === undefined) {
      return undefined;
    }

    return this.requests.request(
      timeoutMs ?? this.registration.requestTimeoutMs,
      signal,
      async (requestOperation) => {
        const effectiveTimeoutMs =
          timeoutMs ?? this.registration.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS;
        throwIfAborted(signal);
        const operation = router.requestToSpot(
          target.targetNodeRid,
          target.spotId,
          parts,
          effectiveTimeoutMs,
          onAdmitted,
          requestOperation
        );
        return waitRequestReply(
          operation,
          'Spot request',
          requestOperation,
          undefined,
          closeMessages
        ).then((replyParts) => {
          try {
            return decodeChannelReply<TReply>(replyParts, codecs);
          } finally {
            closeMessages(replyParts);
            closeMessages(parts);
          }
        });
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
  ): Promise<readonly Message[]> | undefined {
    const router = this.targets.spotNodeRouter(target.routerChannelId);
    if (router === undefined) {
      return undefined;
    }

    return this.requests.request(
      timeoutMs ?? this.registration.requestTimeoutMs,
      signal,
      async (requestOperation) => {
        const effectiveTimeoutMs =
          timeoutMs ?? this.registration.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS;
        throwIfAborted(signal);
        const operation = router.requestToSpot(
          target.targetNodeRid,
          target.spotId,
          request,
          effectiveTimeoutMs,
          onAdmitted,
          requestOperation
        );
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
}
