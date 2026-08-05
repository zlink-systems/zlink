import type { ZLinkFanoutPublishCall } from './Calls';

export interface ZLinkFanoutListenerStatus {
  readonly channelName: string;
  readonly endpoint: string;
  readonly observedAt: Date;
}

export interface ZLinkFanoutClient {
  publish(channelName: string, event: unknown): ZLinkFanoutPublishCall;
  publish(channelName: string, topic: string, event: unknown): ZLinkFanoutPublishCall;
  getListenerStatus(channelName: string): ZLinkFanoutListenerStatus;
}
