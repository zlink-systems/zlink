import type { RoutingId } from '../../contracts';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkLocalSpotRouteDispatcher } from './spot-route-dispatch-strategy';

export class ZLinkLocalSpotRouteTransport {
  constructor(
    private readonly dispatcher: ZLinkLocalSpotRouteDispatcher | undefined,
    private readonly defaultRequestTimeoutMs: number | undefined
  ) {}

  async send(
    routerChannelId: string,
    spotId: RoutingId,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<void> {
    if (this.dispatcher === undefined) {
      throw new ZLinkConfigurationException(`Route channel '${routerChannelId}' could not dispatch local SPOT send.`);
    }
    await this.dispatcher.send(spotId, packetName, message, {
      channelName: routerChannelId,
      signal,
      metadata
    });
  }

  request<TReply>(
    routerChannelId: string,
    spotId: RoutingId,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<TReply> {
    if (this.dispatcher === undefined) {
      throw new ZLinkConfigurationException(`Route channel '${routerChannelId}' could not dispatch local SPOT request.`);
    }
    const pending = this.dispatcher.request<TReply>(spotId, packetName, request, {
      channelName: routerChannelId,
      signal
    });
    const effectiveTimeoutMs = timeoutMs ?? this.defaultRequestTimeoutMs;
    if (effectiveTimeoutMs === undefined) return pending;
    return new Promise<TReply>((resolve, reject) => {
      const timeout = setTimeout(
        () => reject(new ZLinkConfigurationException(`Route channel '${routerChannelId}' local SPOT request timed out.`)),
        effectiveTimeoutMs
      );
      pending.then(
        (value) => { clearTimeout(timeout); resolve(value); },
        (error) => { clearTimeout(timeout); reject(error); }
      );
    });
  }
}
