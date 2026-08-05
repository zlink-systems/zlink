import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendSpotRouteBridge } from '../backend/contracts';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import { closeMessages, decodeChannelReply, type ZLinkChannelEnvelopeCodecRegistry } from './channel-envelope';
import { appendParts } from './channel-multipart';
import type { ZLinkChannelSocketRegistry } from './channel-socket-registry';
import { delay, isTransientRouteNotReadyError } from './route-readiness';
import type { ZLinkSpotRouteBridgeRawReplyRegistry } from './spot-route-bridge-raw-reply';

const ZLINK_SEND_DONT_WAIT = 1;

export class ZLinkSpotRouteBridgeTransport {
  constructor(
    private readonly bridges: ReadonlyMap<string, ZLinkBackendSpotRouteBridge>,
    private readonly sockets: ZLinkChannelSocketRegistry,
    private readonly rawReplies: ZLinkSpotRouteBridgeRawReplyRegistry,
    private readonly defaultRequestTimeoutMs: number | undefined
  ) {}

  has(routerChannelId: string): boolean {
    return this.bridges.has(routerChannelId);
  }

  async send(target: ZLinkSpotRouteTarget, parts: readonly Message[], signal?: AbortSignal): Promise<void> {
    const bridge = this.requireBridge(target.routerChannelId);
    try {
      const submitter = this.sockets.requireSubmitter(this.sockets.routeRouter(target.routerChannelId));
      const deadline = Date.now() + (this.defaultRequestTimeoutMs ?? 30_000);
      for (;;) {
        try {
          await submitter.submitCommand(
            () => appendParts(bridge.send(target.routerChannelId, target.targetNodeRid, target.spotId), parts)
              .flags(ZLINK_SEND_DONT_WAIT).submit(),
            signal
          );
          return;
        } catch (error) {
          if (!isTransientRouteNotReadyError(error) || Date.now() >= deadline) throw error;
          await delay(10, signal);
        }
      }
    } finally {
      closeMessages(parts);
    }
  }

  request<TReply>(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<TReply> {
    const bridge = this.requireBridge(target.routerChannelId);
    return this.sockets.requireSubmitter(this.sockets.routeRouter(target.routerChannelId)).submitRequest(
      (resolve, reject) => {
        const submitted = appendParts(bridge.request(target.routerChannelId, target.targetNodeRid, target.spotId), parts)
          .timeout(timeoutMs ?? 0).flags(ZLINK_SEND_DONT_WAIT).submit((result, replyParts) => {
            try {
              if (result !== 0) {
                reject(new ZLinkConfigurationException(`Route channel '${target.routerChannelId}' spot request failed with result ${result}.`));
                return;
              }
              resolve(decodeChannelReply<TReply>(replyParts as readonly Message[], codecs));
            } catch (error) {
              reject(error);
            } finally {
              closeMessages(replyParts as readonly Message[]);
              closeMessages(parts);
            }
          });
        if (!submitted) {
          closeMessages(parts);
          reject(new ZLinkConfigurationException(`Route channel '${target.routerChannelId}' is not ready for SPOT request.`));
        }
        return submitted;
      }, signal, timeoutMs
    );
  }

  requestRaw(
    target: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<readonly Message[]> {
    const bridge = this.requireBridge(target.routerChannelId);
    const effectiveTimeoutMs = timeoutMs ?? this.defaultRequestTimeoutMs;
    return new Promise<readonly Message[]>((resolve, reject) => {
      const pending = this.rawReplies.enqueue(
        target.routerChannelId, resolve, reject, timeoutMs, this.defaultRequestTimeoutMs, signal
      );
      try {
        const submission = this.sockets.requireSubmitter(this.sockets.routeRouter(target.routerChannelId)).submitRequest<void>(
          (complete, fail) => {
            if (!pending.attachSubmission(complete, fail)) return true;
            return bridge.request(target.routerChannelId, target.targetNodeRid, target.spotId)
              .message(request).timeout(effectiveTimeoutMs ?? 0).submit((result, replyParts) => {
                if (result !== 0) {
                  closeMessages(replyParts as readonly Message[]);
                  pending.reject(new ZLinkConfigurationException(
                    `Route channel '${target.routerChannelId}' spot request failed with result ${result}.`
                  ), true);
                  return;
                }
                pending.resolve(replyParts as readonly Message[]);
              });
          }, undefined, -1
        );
        void submission.catch((error) => pending.reject(error, true));
      } catch (error) {
        pending.reject(error, true);
      }
    });
  }

  private requireBridge(routerChannelId: string): ZLinkBackendSpotRouteBridge {
    const bridge = this.bridges.get(routerChannelId);
    if (bridge === undefined) throw new ZLinkConfigurationException(`Route channel '${routerChannelId}' has no SPOT route bridge.`);
    return bridge;
  }
}
