import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendSendFlags } from '../backend/contracts';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import {
  closeMessages,
  decodeChannelReply,
  type ZLinkChannelEnvelopeCodecRegistry
} from './channel-envelope';
import type { ZLinkChannelSocketRegistry } from './channel-socket-registry';

export class ZLinkRouterSocketSpotRouteTransport {
  constructor(private readonly sockets: ZLinkChannelSocketRegistry) {}

  async send(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    flags: ZLinkBackendSendFlags,
    signal?: AbortSignal
  ): Promise<void> {
    const router = this.sockets.routeRouter(target.routerChannelId);
    try {
      await this.sockets.requireSubmitter(router).submitCommand(
        () => router.sendToSpot(target.targetNodeRid, target.spotId, parts, flags),
        signal
      );
    } finally {
      closeMessages(parts);
    }
  }

  request<TReply>(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
    flags: ZLinkBackendSendFlags,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<TReply> {
    const router = this.sockets.routeRouter(target.routerChannelId);
    return this.sockets.requireSubmitter(router).submitRequest(
      (resolve, reject) => {
        const submitted = router.requestToSpot(
          target.targetNodeRid,
          target.spotId,
          parts,
          (result, replyParts) => {
            try {
              if (result !== 0) {
                reject(this.requestFailure(target.routerChannelId, result));
                return;
              }
              resolve(decodeChannelReply<TReply>(replyParts as readonly Message[], codecs));
            } catch (error) {
              reject(error);
            } finally {
              closeMessages(replyParts as readonly Message[]);
              closeMessages(parts);
            }
          },
          flags,
          timeoutMs
        );
        if (!submitted) {
          closeMessages(parts);
          reject(this.notReady(target.routerChannelId));
        }
        return submitted;
      },
      signal,
      timeoutMs
    );
  }

  requestRaw(
    target: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<readonly Message[]> {
    const router = this.sockets.routeRouter(target.routerChannelId);
    return this.sockets.requireSubmitter(router).submitRequest(
      (resolve, reject) => {
        const submitted = router.requestToSpot(
          target.targetNodeRid,
          target.spotId,
          [request],
          (result, replyParts) => {
            if (result !== 0) {
              closeMessages(replyParts as readonly Message[]);
              reject(this.requestFailure(target.routerChannelId, result));
              return;
            }
            resolve(replyParts as readonly Message[]);
          },
          0,
          timeoutMs
        );
        if (!submitted) reject(this.notReady(target.routerChannelId));
        return submitted;
      },
      signal,
      timeoutMs
    );
  }

  private requestFailure(routerChannelId: string, result: number): ZLinkConfigurationException {
    return new ZLinkConfigurationException(
      `Route channel '${routerChannelId}' spot request failed with result ${result}.`
    );
  }

  private notReady(routerChannelId: string): ZLinkConfigurationException {
    return new ZLinkConfigurationException(
      `Route channel '${routerChannelId}' is not ready for SPOT request.`
    );
  }
}
