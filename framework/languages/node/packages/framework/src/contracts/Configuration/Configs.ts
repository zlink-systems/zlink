import type { RoutingId } from '../Common';

export interface ZLinkSocketConfig {
  bind?: string;
  connect?: string;
  channelName?: string;
  weight?: number;
  sendHighWaterMark?: number;
  receiveHighWaterMark?: number;
  sendTimeoutMs?: number;
  maxMessageSize?: number;
}

export interface ZLinkRouteConfig {
  channelName: string;
  endpoint: string;
}

export interface ZLinkOutboundRouteConfig {
  targetNodeRid: RoutingId;
  endpoint: string;
}

export interface ZLinkSpotPublisherConfig {
  sendHighWaterMark: number;
  sendTimeoutMs?: number;
  lingerMs?: number;
}

export interface ZLinkSpotSubscriberConfig {
  topic: string;
}
