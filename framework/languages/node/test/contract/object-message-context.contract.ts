import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkEntrySpot,
  ZLinkEntrySpotActorRequestHandler,
  ZLinkMessageContext,
  ZLinkPublishMessageContext,
  ZLinkRouteMessageContext,
  ZLinkSpot,
  ZLinkSpotActorSendHandler,
  ZLinkSpotContext
} from '@zlink-systems/framework';

function exactContextSurface(
  actorContext: ZLinkActorContext,
  spotContext: ZLinkSpotContext,
  messageContext: ZLinkMessageContext
): void {
  const actorId: string = actorContext.actorId;
  const actorGeneration: bigint = actorContext.objectGeneration;
  const spotGeneration: number = spotContext.objectGeneration;
  void [actorId, actorGeneration, spotGeneration, messageContext];

  // @ts-expect-error Actor handler registration belongs to the containing Spot.
  actorContext.handlers;
  // @ts-expect-error Actor leave is initiated by the containing Spot context.
  actorContext.leaveSpot();
  // @ts-expect-error routingId is not a public Spot context identity alias.
  spotContext.routingId;
  // @ts-expect-error Filters receive the message context directly, without an owner wrapper.
  messageContext.ownerKind;
}

class ExactActor implements ZLinkActor {
  constructor(readonly context: ZLinkActorContext) {}
}

class ExactActorFactory implements ZLinkActorFactory<ExactActor> {
  async create(context: ZLinkActorContext, _signal?: AbortSignal): Promise<ExactActor> {
    return new ExactActor(context);
  }
}

class ExactSpot implements ZLinkSpot<ExactActor> {
  declare readonly context: ZLinkSpotContext<ExactActor, ExactSpot>;
  async onActorJoin(): Promise<{ accepted: boolean }> { return { accepted: true }; }
  async onJoinedActor(): Promise<void> {}
  async onLeaveActor(): Promise<void> {}
  async onDisconnectActor(): Promise<void> {}
}

class ExactEntrySpot implements ZLinkEntrySpot<ExactActor> {
  declare readonly context: import('@zlink-systems/framework').ZLinkEntrySpotContext<
    ExactActor,
    ExactEntrySpot
  >;
  async onJoinedActor(): Promise<void> {}
  async onLeaveActor(): Promise<void> {}
  async onDisconnectActor(): Promise<void> {}
}

class ExactSpotActorSendHandler
implements ZLinkSpotActorSendHandler<ExactSpot, ExactActor, string> {
  async handle(
    _spot: ExactSpot,
    _actor: ExactActor,
    _context: ZLinkMessageContext,
    _message: string
  ): Promise<void> {}
}

class ExactEntryActorRequestHandler
implements ZLinkEntrySpotActorRequestHandler<ExactEntrySpot, ExactActor, string, number> {
  async handle(
    _spot: ExactEntrySpot,
    _actor: ExactActor,
    _context: ZLinkMessageContext,
    request: string
  ): Promise<number> {
    return request.length;
  }
}

function specializedContexts(
  route: ZLinkRouteMessageContext,
  publish: ZLinkPublishMessageContext
): void {
  const meshName: string = route.meshName;
  const sourceNodeRid: string = route.sourceNodeRid;
  const channelName: string = publish.channelName;
  void [meshName, sourceNodeRid, channelName];
  // @ts-expect-error Route contexts no longer expose the transport router alias.
  route.routerChannelId;
  // @ts-expect-error Cancellation is owned by dispatch, not the message context.
  publish.connectionAborted;
}

void [
  exactContextSurface,
  ExactActorFactory,
  ExactSpot,
  ExactEntrySpot,
  ExactSpotActorSendHandler,
  ExactEntryActorRequestHandler,
  specializedContexts
];
