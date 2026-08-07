import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendSpot } from '../backend/contracts';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import { throwIfAborted, ZLinkDeferredCompletion } from '../abort';
import { ZLinkConfigurationException } from '../configuration';
import { closeMessages, ZLinkChannelMessageKind } from './channel-envelope';
import { decodeSpotDirectReply, encodeSpotDirectEnvelope } from './spot-direct-envelope';
import { delay } from './route-readiness';

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
    const completion = new ZLinkDeferredCompletion<TReply>();
    void this.submitWhenReady(
      (remainingMs) => sourceSpot.requestToSpot(
        target.targetNodeRid,
        target.spotId,
        parts,
        (result, replyParts) => {
          try {
            if (result !== 0) {
              completion.reject(this.requestFailure(target.routerChannelId, result));
              return;
            }
            completion.resolve(decodeSpotDirectReply<TReply>(replyParts as readonly Message[]));
          } catch (error) {
            completion.reject(error);
          } finally {
            closeMessages(replyParts as readonly Message[]);
          }
        },
        0,
        remainingMs
      ),
      timeoutMs,
      signal,
      this.notReady(target.routerChannelId, 'request')
    ).catch((error) => completion.reject(error));
    return completion.wait(signal).finally(() => closeMessages(parts));
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
      await this.submitWhenReady(
        () => sourceSpot.sendToSpot(target.targetNodeRid, target.spotId, parts, 0),
        timeoutMs,
        signal,
        this.notReady(target.routerChannelId, 'send')
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
    const completion = new ZLinkDeferredCompletion<readonly Message[]>();
    void this.submitWhenReady(
      (remainingMs) => sourceSpot.requestToSpot(
        target.targetNodeRid,
        target.spotId,
        request,
        (result, replyParts) => {
          if (result !== 0) {
            closeMessages(replyParts as readonly Message[]);
            completion.reject(this.requestFailure(target.routerChannelId, result));
            return;
          }
          if (!completion.resolve(replyParts as readonly Message[])) {
            closeMessages(replyParts as readonly Message[]);
          }
        },
        0,
        remainingMs
      ),
      timeoutMs,
      signal,
      this.notReady(target.routerChannelId, 'request')
    ).catch((error) => completion.reject(error));
    return completion.wait(signal);
  }

  private async submitWhenReady(
    submit: (remainingMs: number) => boolean,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    notReadyError: ZLinkConfigurationException
  ): Promise<void> {
    const effectiveTimeoutMs = timeoutMs ?? this.defaultRequestTimeoutMs ?? 30_000;
    const deadline = Date.now() + effectiveTimeoutMs;
    for (;;) {
      throwIfAborted(signal);
      const remainingMs = Math.max(1, deadline - Date.now());
      if (submit(remainingMs)) return;
      if (Date.now() >= deadline) throw notReadyError;
      await delay(Math.min(10, remainingMs), signal);
    }
  }

  private requestFailure(routerChannelId: string, result: number): ZLinkConfigurationException {
    return new ZLinkConfigurationException(
      `SpotNode router '${routerChannelId}' spot request failed with result ${result}.`
    );
  }

  private notReady(routerChannelId: string, operation: 'request' | 'send'): ZLinkConfigurationException {
    return new ZLinkConfigurationException(
      `SpotNode router '${routerChannelId}' is not ready for SPOT ${operation}.`
    );
  }
}
