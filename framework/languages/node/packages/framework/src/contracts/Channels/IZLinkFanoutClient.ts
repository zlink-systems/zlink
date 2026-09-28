import type { ZLinkFanoutPublishCall } from './Calls';

export interface ZLinkFanoutClient {
  publish(channelName: string, event: unknown): ZLinkFanoutPublishCall;
  publish(channelName: string, topic: string, event: unknown): ZLinkFanoutPublishCall;
}
