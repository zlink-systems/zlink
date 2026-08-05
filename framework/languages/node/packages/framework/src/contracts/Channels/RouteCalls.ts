import type { RoutingId } from '../Common';
import type { ZLinkChannelRequestCall, ZLinkPublishCall, ZLinkRequestCall, ZLinkSendCall } from './Calls';

export interface ZLinkRouteClient {
  sendToNode(meshName: string, targetNodeRid: RoutingId, message: unknown): ZLinkSendCall;
  requestToNode(meshName: string, targetNodeRid: RoutingId, request: unknown): ZLinkRequestCall;
  sendToChannel(channelName: string, message: unknown): ZLinkSendCall;
  requestToChannel(channelName: string, request: unknown): ZLinkChannelRequestCall;
}

export interface ZLinkSpotPublisherClient {
  publish(meshName: string, channelName: string, topic: string, event: unknown): ZLinkPublishCall;
}
