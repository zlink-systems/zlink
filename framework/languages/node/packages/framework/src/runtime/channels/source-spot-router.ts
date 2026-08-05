import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendSpot } from '../backend/contracts';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import { createAbortError, throwIfAborted } from '../abort';
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
    return new Promise<TReply>((resolve, reject) => {
      let settled = false;
      const cleanup = () => signal?.removeEventListener('abort', onAbort);
      const complete = (value: TReply) => {
        if (settled) return;
        settled = true;
        cleanup();
        resolve(value);
      };
      const fail = (error: unknown) => {
        if (settled) return;
        settled = true;
        cleanup();
        reject(error);
      };
      const onAbort = () => fail(createAbortError());
      signal?.addEventListener('abort', onAbort, { once: true });
      try {
        void this.submitWhenReady(
          (remainingMs) => sourceSpot.requestToSpot(
            target.targetNodeRid,
            target.spotId,
            parts,
            (result, replyParts) => {
              try {
                if (result !== 0) {
                  fail(this.requestFailure(target.routerChannelId, result));
                  return;
                }
                complete(decodeSpotDirectReply<TReply>(replyParts as readonly Message[]));
              } catch (error) {
                fail(error);
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
        ).catch(fail);
      } catch (error) {
        fail(error);
      }
    }).finally(() => closeMessages(parts));
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
    return new Promise<readonly Message[]>((resolve, reject) => {
      let settled = false;
      const cleanup = () => signal?.removeEventListener('abort', onAbort);
      const complete = (value: readonly Message[]): boolean => {
        if (settled) return false;
        settled = true;
        cleanup();
        resolve(value);
        return true;
      };
      const fail = (error: unknown) => {
        if (settled) return;
        settled = true;
        cleanup();
        reject(error);
      };
      const onAbort = () => fail(createAbortError());
      signal?.addEventListener('abort', onAbort, { once: true });
      try {
        void this.submitWhenReady(
          (remainingMs) => sourceSpot.requestToSpot(
            target.targetNodeRid,
            target.spotId,
            request,
            (result, replyParts) => {
              if (result !== 0) {
                closeMessages(replyParts as readonly Message[]);
                fail(this.requestFailure(target.routerChannelId, result));
                return;
              }
              if (!complete(replyParts as readonly Message[])) {
                closeMessages(replyParts as readonly Message[]);
              }
            },
            0,
            remainingMs
          ),
          timeoutMs,
          signal,
          this.notReady(target.routerChannelId, 'request')
        ).catch(fail);
      } catch (error) {
        fail(error);
      }
    });
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
