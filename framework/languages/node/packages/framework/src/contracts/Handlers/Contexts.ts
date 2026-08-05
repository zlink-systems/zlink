import type { RoutingId, ZLinkMessageMetadata } from '../Common';

export interface ZLinkMessageContext {
  readonly meshName?: string;
  readonly channelName?: string;
  readonly packetName: string;
  readonly contentType?: string;
  readonly metadata: ZLinkMessageMetadata;
  readonly correlationId?: string;
}

export interface ZLinkPublishMessageContext extends ZLinkMessageContext {
  readonly channelName: string;
  readonly topic: string;
  readonly source?: string;
}

export interface ZLinkRouteMessageContext extends ZLinkMessageContext {
  readonly meshName: string;
  readonly sourceNodeRid: RoutingId;
}
