import { waitRequestReply } from '../messaging/request-deadline';
import { RequestRuntime } from '../foundation/request-runtime';
import type { PendingOperation } from '../foundation/operation-registry';
import type { RoutingId } from '../../contracts';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkLocalSpotRouteDispatcher } from './spot-route-dispatch-strategy';

export class ZLinkLocalSpotRouteTransport {
  constructor(
    private readonly dispatcher: ZLinkLocalSpotRouteDispatcher | undefined,
    private readonly defaultRequestTimeoutMs: number | undefined,
    private readonly requests = new RequestRuntime()
  ) {}

  async send(
    routerChannelId: string,
    spotId: RoutingId,
    packetName: string | undefined,
    message: unknown,
    metadata?: ReadonlyMap<string, string>
  ): Promise<void> {
    if (this.dispatcher === undefined) {
      throw new ZLinkConfigurationException(
        `Route channel '${routerChannelId}' could not dispatch local SPOT send.`
      );
    }
    await this.dispatcher.send(spotId, packetName, message, {
      channelName: routerChannelId,
      metadata
    });
  }

  request<TReply>(
    routerChannelId: string,
    spotId: RoutingId,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>
  ): Promise<TReply> {
    if (this.dispatcher === undefined) {
      throw new ZLinkConfigurationException(
        `Route channel '${routerChannelId}' could not dispatch local SPOT request.`
      );
    }
    const effectiveTimeoutMs = timeoutMs ?? this.defaultRequestTimeoutMs;
    const dispatcher = this.dispatcher;
    return this.requests.request(
      effectiveTimeoutMs,
      signal,
      async (requestOperation) => {
        const pending = dispatcher.request<TReply>(spotId, packetName, request, {
          channelName: routerChannelId,
          onAdmitted,
          signal,
          requestOperation
        });
        return waitRequestReply(pending, 'Local Spot request', requestOperation);
      },
      existing
    );
  }
}
