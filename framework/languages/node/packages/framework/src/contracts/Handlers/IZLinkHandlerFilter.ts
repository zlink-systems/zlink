import type { ZLinkHandlerFilterNext } from './ZLinkHandlerFilterNext';
import type { ZLinkMessageContext } from './Contexts';

export enum ZLinkHandlerDispatchKind {
  NodeDirectSend = 'nodeDirectSend',
  NodeDirectRequest = 'nodeDirectRequest',
  ChannelSend = 'channelSend',
  ChannelRequest = 'channelRequest',
  ClassicFanout = 'classicFanout'
}

export interface ZLinkHandlerFilterContext extends ZLinkMessageContext {
  readonly dispatchKind: ZLinkHandlerDispatchKind;
}

export interface ZLinkHandlerFilter {
  invoke(
    context: ZLinkHandlerFilterContext,
    next: ZLinkHandlerFilterNext,
    signal?: AbortSignal
  ): Promise<void>;
}
