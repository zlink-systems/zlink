import type { ZLinkMessageMetadata } from '../Common';

export interface ZLinkSendCall {
  metadata(key: string, value: string): this;
  metadata(metadata: ZLinkMessageMetadata): this;
  submit(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkRequestCall {
  metadata(key: string, value: string): this;
  metadata(metadata: ZLinkMessageMetadata): this;
  timeout(timeoutMs: number): this;
  submit<TReply>(signal?: AbortSignal): Promise<TReply>;
}

export interface ZLinkChannelRequestCall extends ZLinkRequestCall {
  yield<TReply>(signal?: AbortSignal): Promise<TReply>;
}

export interface ZLinkPublishCall {
  metadata(key: string, value: string): this;
  metadata(metadata: ZLinkMessageMetadata): this;
  /**
   * Aggregates submission to the selected remote routes' source-local
   * transport queues and matching local Spot queues. Completion does not
   * wait for remote Spot queue admission or subscriber handler execution.
   */
  submit(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkFanoutPublishCall {
  submit(signal?: AbortSignal): Promise<void>;
}
