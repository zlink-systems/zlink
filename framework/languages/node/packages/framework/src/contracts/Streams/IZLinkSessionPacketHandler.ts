import type { Type, ZLinkMessage } from '../Common';
import type { ZLinkSessionDispatchContext } from './IZLinkSession';

export interface ZLinkSessionPacketHandler<TSessionContext, TMessage = ZLinkMessage> {
  handle(context: TSessionContext, dispatch: ZLinkSessionDispatchContext, message: TMessage): Promise<void>;
}

export interface ZLinkSessionHandlerRegistry {
  addHandler<THandler>(handlerType: Type<THandler>): this;
  tryHandle(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<boolean>;
}
