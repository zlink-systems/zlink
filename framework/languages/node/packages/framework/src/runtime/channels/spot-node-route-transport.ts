import { RequestRuntime } from '../foundation/request-runtime';
import type { PendingOperation, OperationObserver } from '../foundation/operation-registry';
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
    existing?: PendingOperation<unknown>,
    destination?: OperationObserver<unknown, unknown>
  ): Promise<TReply> | undefined {
    const router = this.targets.spotNodeRouter(target.routerChannelId);
    if (router === undefined) {
      return undefined;
    }

    const operation = this.requests.begin(
      timeoutMs ?? this.registration.requestTimeoutMs,
      signal,
      existing
    );
    return this.requests.run<TReply>(
      operation,
      (receiver) => {
        throwIfAborted(signal);
        router.requestToSpot(
          target.targetNodeRid,
          target.spotId,
          parts,
          timeoutMs ?? this.registration.requestTimeoutMs,
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

    const operation = this.requests.begin(
      timeoutMs ?? this.registration.requestTimeoutMs,
      signal,
      existing
    );
    return this.requests.run<readonly Message[]>(
      operation,
      (receiver) => {
        throwIfAborted(signal);
        router.requestToSpot(
          target.targetNodeRid,
          target.spotId,
          request,
          timeoutMs ?? this.registration.requestTimeoutMs,
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
