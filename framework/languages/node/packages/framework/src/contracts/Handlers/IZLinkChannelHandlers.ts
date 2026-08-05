import type { ZLinkActor } from '../Actors';
import type { ZLinkEntrySpot } from '../Spots';
import type { ZLinkTimerTick } from '../Timers';
import type {
  ZLinkMessageContext,
  ZLinkPublishMessageContext,
  ZLinkRouteMessageContext
} from './Contexts';

export interface ZLinkRequestHandler<TRequest, TResponse> {
  handle(request: TRequest, context: ZLinkMessageContext): Promise<TResponse>;
}

export interface ZLinkSendHandler<TMessage> {
  handle(message: TMessage, context: ZLinkMessageContext): Promise<void>;
}

export interface ZLinkRouteSendHandler<TMessage> {
  handle(message: TMessage, context: ZLinkRouteMessageContext): Promise<void>;
}

export interface ZLinkRouteRequestHandler<TRequest, TReply> {
  handle(request: TRequest, context: ZLinkRouteMessageContext): Promise<TReply>;
}

export interface ZLinkFanoutHandler<TMessage> {
  handle(message: TMessage, context: ZLinkPublishMessageContext): Promise<void>;
}

export interface ZLinkSpotPacketHandler<TSpot, TMessage> {
  handle(spot: TSpot, message: TMessage, context: ZLinkMessageContext): Promise<void>;
}

export interface ZLinkSpotRequestHandler<TSpot, TRequest, TReply> {
  handle(spot: TSpot, request: TRequest, context: ZLinkMessageContext): Promise<TReply>;
}

export interface ZLinkSpotSubscriptionHandler<TSpot, TEvent> {
  handle(spot: TSpot, event: TEvent, context: ZLinkPublishMessageContext): Promise<void>;
}

export interface ZLinkSpotTimerHandler<TSpot> {
  handle(spot: TSpot, tick: ZLinkTimerTick): Promise<void>;
}

export interface ZLinkSpotActorSendHandler<TSpot, TActor extends ZLinkActor, TMessage> {
  handle(spot: TSpot, actor: TActor, context: ZLinkMessageContext, message: TMessage): Promise<void>;
}

export interface ZLinkSpotActorRequestHandler<TSpot, TActor extends ZLinkActor, TRequest, TReply> {
  handle(spot: TSpot, actor: TActor, context: ZLinkMessageContext, request: TRequest): Promise<TReply>;
}

export interface ZLinkEntrySpotActorSendHandler<
  TEntrySpot extends ZLinkEntrySpot<TActor>,
  TActor extends ZLinkActor,
  TMessage
> {
  handle(spot: TEntrySpot, actor: TActor, context: ZLinkMessageContext, message: TMessage): Promise<void>;
}

export interface ZLinkEntrySpotActorRequestHandler<
  TEntrySpot extends ZLinkEntrySpot<TActor>,
  TActor extends ZLinkActor,
  TRequest,
  TReply
> {
  handle(spot: TEntrySpot, actor: TActor, context: ZLinkMessageContext, request: TRequest): Promise<TReply>;
}
