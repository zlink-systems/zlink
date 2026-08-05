import type { ZLinkChannelRequestCall, ZLinkSendCall } from './Calls';

export interface ZLinkChannelClient {
  sendToChannel(channelName: string, message: unknown): ZLinkSendCall;
  requestToChannel(channelName: string, request: unknown): ZLinkChannelRequestCall;
}
