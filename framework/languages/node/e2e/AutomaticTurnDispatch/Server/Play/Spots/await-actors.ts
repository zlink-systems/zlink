import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ROUTE_CLIENT } from '@zlink-systems/nestjs';
import { DelayReq } from '../../../Shared/messages';
import type {
  ActorJoinAwaitReq,
  ActorPushAwaitReq,
  ActorAwaitRes,
  ActorFastMsg,
  ActorFastReq,
  ActorAwaitReq,
  DeferredJoinFailureMsg,
  DelayRes
} from '../../../Shared/messages';
import { ActorPushNotify, AutomaticTurnDispatchNames } from '../../../Shared/messages';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkRouteClient,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage,
  ZLinkMessageContext,
  ZLinkSpotActorRequestHandler,
  ZLinkSpotActorSendHandler,
  ZLinkSpotActorJoinResult
} from '@zlink-systems/framework';
import { ZLinkSpotActorRequest, ZLinkSpotActorSend } from '@zlink-systems/framework';
import {
  zlinkEntrySpotActorRequestHandler,
  zlinkEntrySpotActorSendHandler,
  zlinkSpotActorRequestHandler,
  zlinkSpotActorSendHandler
} from '@zlink-systems/nestjs';
import { EvidenceStore } from '../Support/evidence-store';
import { AwaitProbeSpot } from './await-probe-spot';

export class AwaitActor implements ZLinkActor {
  constructor(readonly actorId: string, readonly context: ZLinkActorContext) {}
}

@Injectable()
export class AwaitActorFactory implements ZLinkActorFactory {
  async create(context: ZLinkActorContext): Promise<AwaitActor> {
    return new AwaitActor(context.actorId, context);
  }
}

@Injectable()
export class AwaitEntrySpot implements ZLinkEntrySpot<AwaitActor> {
  readonly context!: ZLinkEntrySpotContext<AwaitActor>;

  async onActorJoin(actorId: string, request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult> {
    void actorId;
    void request;
    return { accepted: true };
  }

  async onJoinedActor(actor: AwaitActor): Promise<void> { void actor; }

  async onLeaveActor(actor: AwaitActor): Promise<void> { void actor; }

  async onDisconnectActor(actor: AwaitActor): Promise<void> { void actor; }
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  entrySpot: () => AwaitEntrySpot,
  actor: () => AwaitActor,
  packetName: 'ActorAwaitReq'
})
export class EntryActorAwaitHandler
{
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly route: ZLinkRouteClient
  ) {}

  @ZLinkSpotActorRequest('ActorAwaitReq')
  async handle(
    _spot: AwaitEntrySpot,
    actor: AwaitActor,
    context: ZLinkMessageContext,
    request: ActorAwaitReq
  ): Promise<ActorAwaitRes> {
    void context;
    const target = actorEvidenceTarget(this.evidence, actor);
    await recordActorAwaitEvidence(this.evidence, this.route, target, actor, request);
    return actorReply('TD-D', request.requestId, actor, target, 'actor-await-completed');
  }
}

@Injectable()
@zlinkSpotActorRequestHandler({
  spot: () => AwaitProbeSpot,
  actor: () => AwaitActor,
  packetName: 'ActorAwaitReq'
})
export class SpotActorAwaitHandler
  implements ZLinkSpotActorRequestHandler<AwaitProbeSpot, AwaitActor, ActorAwaitReq, ActorAwaitRes> {
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly route: ZLinkRouteClient
  ) {}

  @ZLinkSpotActorRequest('ActorAwaitReq')
  async handle(
    _spot: AwaitProbeSpot,
    actor: AwaitActor,
    context: ZLinkMessageContext,
    request: ActorAwaitReq
  ): Promise<ActorAwaitRes> {
    void context;
    const target = actorEvidenceTarget(this.evidence, actor);
    await recordActorAwaitEvidence(this.evidence, this.route, target, actor, request);
    return actorReply('TD-D', request.requestId, actor, target, 'actor-await-completed');
  }
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  entrySpot: () => AwaitEntrySpot,
  actor: () => AwaitActor,
  packetName: 'ActorFastReq'
})
export class EntryActorFastHandler
{
  constructor(private readonly evidence: EvidenceStore) {}

  @ZLinkSpotActorRequest('ActorFastReq')
  async handle(
    _spot: AwaitEntrySpot,
    actor: AwaitActor,
    context: ZLinkMessageContext,
    request: ActorFastReq
  ): Promise<ActorAwaitRes> {
    void context;
    const target = actorEvidenceTarget(this.evidence, actor);
    recordActorFastEvidence(this.evidence, target, actor, request);
    return actorReply('TD-D', request.requestId, actor, target, request.marker);
  }
}

@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => AwaitEntrySpot,
  actor: () => AwaitActor,
  packetName: 'ActorFastMsg'
})
export class EntryActorFastSendHandler
{
  constructor(private readonly evidence: EvidenceStore) {}

  @ZLinkSpotActorSend('ActorFastMsg')
  async handle(
    _spot: AwaitEntrySpot,
    actor: AwaitActor,
    context: ZLinkMessageContext,
    request: ActorFastMsg
  ): Promise<void> {
    void context;
    recordActorFastEvidence(this.evidence, actorEvidenceTarget(this.evidence, actor), actor, request);
  }
}

@Injectable()
@zlinkSpotActorRequestHandler({
  spot: () => AwaitProbeSpot,
  actor: () => AwaitActor,
  packetName: 'ActorFastReq'
})
export class SpotActorFastHandler
  implements ZLinkSpotActorRequestHandler<AwaitProbeSpot, AwaitActor, ActorFastReq, ActorAwaitRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  @ZLinkSpotActorRequest('ActorFastReq')
  async handle(
    _spot: AwaitProbeSpot,
    actor: AwaitActor,
    context: ZLinkMessageContext,
    request: ActorFastReq
  ): Promise<ActorAwaitRes> {
    void context;
    const target = actorEvidenceTarget(this.evidence, actor);
    recordActorFastEvidence(this.evidence, target, actor, request);
    return actorReply('TD-D', request.requestId, actor, target, request.marker);
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => AwaitProbeSpot,
  actor: () => AwaitActor,
  packetName: 'ActorFastMsg'
})
export class SpotActorFastSendHandler
  implements ZLinkSpotActorSendHandler<AwaitProbeSpot, AwaitActor, ActorFastMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  @ZLinkSpotActorSend('ActorFastMsg')
  async handle(
    _spot: AwaitProbeSpot,
    actor: AwaitActor,
    context: ZLinkMessageContext,
    request: ActorFastMsg
  ): Promise<void> {
    void context;
    recordActorFastEvidence(this.evidence, actorEvidenceTarget(this.evidence, actor), actor, request);
  }
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  entrySpot: () => AwaitEntrySpot,
  actor: () => AwaitActor,
  packetName: 'ActorPushAwaitReq'
})
export class EntryActorPushAwaitHandler
{
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly route: ZLinkRouteClient
  ) {}

  @ZLinkSpotActorRequest('ActorPushAwaitReq')
  async handle(
    _spot: AwaitEntrySpot,
    actor: AwaitActor,
    context: ZLinkMessageContext,
    request: ActorPushAwaitReq
  ): Promise<ActorAwaitRes> {
    void context;
    const target = actorEvidenceTarget(this.evidence, actor);
    await recordActorPushAwaitEvidence(this.evidence, this.route, target, actor, request, false);
    return actorReply('TD-F3', request.requestId, actor, target, 'actor-push-await-completed');
  }
}

@Injectable()
@zlinkSpotActorRequestHandler({
  spot: () => AwaitProbeSpot,
  actor: () => AwaitActor,
  packetName: 'ActorPushAwaitReq'
})
export class SpotActorPushAwaitHandler
  implements ZLinkSpotActorRequestHandler<AwaitProbeSpot, AwaitActor, ActorPushAwaitReq, ActorAwaitRes> {
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly route: ZLinkRouteClient
  ) {}

  @ZLinkSpotActorRequest('ActorPushAwaitReq')
  async handle(
    _spot: AwaitProbeSpot,
    actor: AwaitActor,
    context: ZLinkMessageContext,
    request: ActorPushAwaitReq
  ): Promise<ActorAwaitRes> {
    void context;
    const target = actorEvidenceTarget(this.evidence, actor);
    await recordActorPushAwaitEvidence(this.evidence, this.route, target, actor, request, true);
    return actorReply('TD-F3', request.requestId, actor, target, 'actor-push-await-completed');
  }
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  entrySpot: () => AwaitEntrySpot,
  actor: () => AwaitActor,
  packetName: 'ActorJoinAwaitReq'
})
export class EntryActorJoinAwaitHandler
{
  constructor(private readonly evidence: EvidenceStore) {}

  @ZLinkSpotActorRequest('ActorJoinAwaitReq')
  async handle(
    _spot: AwaitEntrySpot,
    actor: AwaitActor,
    context: ZLinkMessageContext,
    request: ActorJoinAwaitReq
  ): Promise<ActorAwaitRes> {
    void context;
    const target = actorEvidenceTarget(this.evidence, actor);
    await recordActorJoinEvidence(this.evidence, target, actor, request, 'actor-join-await-completed');
    return actorReply('TD-E1', request.requestId, actor, target, 'actor-join-await-completed');
  }
}

@Injectable()
@zlinkSpotActorRequestHandler({
  spot: () => AwaitProbeSpot,
  actor: () => AwaitActor,
  packetName: 'ActorJoinAwaitReq'
})
export class SpotActorJoinAwaitHandler
  implements ZLinkSpotActorRequestHandler<AwaitProbeSpot, AwaitActor, ActorJoinAwaitReq, ActorAwaitRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  @ZLinkSpotActorRequest('ActorJoinAwaitReq')
  async handle(
    _spot: AwaitProbeSpot,
    actor: AwaitActor,
    context: ZLinkMessageContext,
    request: ActorJoinAwaitReq
  ): Promise<ActorAwaitRes> {
    void context;
    const target = actorEvidenceTarget(this.evidence, actor);
    await recordActorJoinEvidence(this.evidence, target, actor, request, 'actor-join-completed');
    return actorReply('TD-E', request.requestId, actor, target, 'actor-join-completed');
  }
}

@Injectable()
@zlinkSpotActorRequestHandler({
  spot: () => AwaitProbeSpot,
  actor: () => AwaitActor,
  packetName: 'DeferredJoinFailureMsg'
})
export class SpotActorDeferredJoinFailureHandler
  implements ZLinkSpotActorRequestHandler<AwaitProbeSpot, AwaitActor, DeferredJoinFailureMsg, ActorAwaitRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  @ZLinkSpotActorRequest('DeferredJoinFailureMsg')
  async handle(
    spot: AwaitProbeSpot,
    actor: AwaitActor,
    context: ZLinkMessageContext,
    request: DeferredJoinFailureMsg
  ): Promise<ActorAwaitRes> {
    void context;
    if (actor.actorId !== request.firstActorId) {
      throw new Error(`TD-E2A first Actor '${request.firstActorId}' was not the handler owner.`);
    }
    const second = spot.findActor(request.secondActorId);
    if (second === undefined) {
      throw new Error(`TD-E2A source Actor '${request.secondActorId}' was not a member.`);
    }
    actor.context.joinSpot(
      request.firstTargetSpotId,
      new DelayReq(request.requestId, 25, 'td-e2a-first')
    ).timeout(5000).defer();
    second.context.joinSpot(
      request.secondTargetSpotId,
      new DelayReq(request.requestId, 25, 'td-e2a-second')
    ).timeout(5000).defer();
    this.evidence.add(
      `deferred-join-failure-registered|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
      + `|request=${request.requestId}|mode=${request.failureMode}`
    );
    if (request.failureMode === 'cancel') {
      const error = new Error('TD-E2A cancellation fixture.');
      error.name = 'AbortError';
      throw error;
    }
    throw new Error('TD-E2A exception fixture.');
  }
}

interface ActorEvidenceTarget {
  readonly spotId: unknown;
  readonly nodeRid: unknown;
}

function actorEvidenceTarget(evidence: EvidenceStore, actor: AwaitActor): ActorEvidenceTarget {
  return {
    spotId: actor.context.spotId ?? evidence.rid,
    nodeRid: evidence.rid
  };
}

async function recordActorAwaitEvidence(
  evidence: EvidenceStore,
  route: ZLinkRouteClient,
  target: ActorEvidenceTarget,
  actor: AwaitActor,
  request: ActorAwaitReq
): Promise<void> {
  const terminator = request.terminator ?? 'async';
  const mailboxId = `actor:${actor.actorId}`;
  evidence.add(
    `actor-await-started|rid=${evidence.rid}|spot=${target.spotId}`
    + `|actor=${actor.actorId}|mailbox=${mailboxId}|request=${request.requestId}`
    + `|delayMs=${request.delayMs}|handler=actor`
  );
  const call = route
    .requestToChannel(AutomaticTurnDispatchNames.delayChannel,
      new DelayReq(request.requestId, request.delayMs, `actor-${actor.actorId}`))
    .timeout(30000);
  evidence.add(
    `actor-await-${terminator === 'yield' ? 'released' : 'held'}|rid=${evidence.rid}|spot=${target.spotId}`
    + `|actor=${actor.actorId}|mailbox=${mailboxId}|request=${request.requestId}|handler=actor`
  );
  if (terminator === 'yield') {
    await call.yield<DelayRes>();
  } else {
    await call.submit<DelayRes>();
  }
  evidence.add(
    `actor-await-resumed|rid=${evidence.rid}|spot=${target.spotId}`
    + `|actor=${actor.actorId}|mailbox=${mailboxId}|request=${request.requestId}|handler=actor`
  );
  evidence.add(
    `actor-await-completed|rid=${evidence.rid}|spot=${target.spotId}`
    + `|actor=${actor.actorId}|mailbox=${mailboxId}|request=${request.requestId}|handler=actor`
  );
}

async function recordActorJoinEvidence(
  evidence: EvidenceStore,
  target: ActorEvidenceTarget,
  actor: AwaitActor,
  request: ActorJoinAwaitReq,
  completedMarker: string
): Promise<void> {
  const mailboxId = `actor:${actor.actorId}`;
  evidence.add(
    `actor-join-started|rid=${evidence.rid}|spot=${target.spotId}`
    + `|actor=${actor.actorId}|mailbox=${mailboxId}|request=${request.requestId}|target=${request.targetSpotId}`
  );
  actor.context
    .joinSpot(request.targetSpotId, new DelayReq(request.requestId, 0, 'join'))
    .timeout(5000)
    .defer();
  evidence.add(
    `${completedMarker}|rid=${evidence.rid}|spot=${target.spotId}`
    + `|actor=${actor.actorId}|mailbox=${mailboxId}|request=${request.requestId}`
    + `|target=${request.targetSpotId}|accepted=true|deferred=true`
  );
}

async function recordActorPushAwaitEvidence(
  evidence: EvidenceStore,
  route: ZLinkRouteClient,
  target: ActorEvidenceTarget,
  actor: AwaitActor,
  request: ActorPushAwaitReq,
  useAwait: boolean
): Promise<void> {
  const mailboxId = `actor:${actor.actorId}`;
  evidence.add(
    `actor-push-await-started|rid=${evidence.rid}|spot=${target.spotId}`
    + `|actor=${actor.actorId}|mailbox=${mailboxId}|request=${request.requestId}|handler=actor`
  );
  const call = route
    .requestToChannel(AutomaticTurnDispatchNames.delayChannel,
      new DelayReq(request.requestId, request.delayMs, `actor-push-${actor.actorId}`))
    .timeout(5000);
  evidence.add(
    `actor-push-await-released|rid=${evidence.rid}|spot=${target.spotId}`
    + `|actor=${actor.actorId}|mailbox=${mailboxId}|request=${request.requestId}|handler=actor`
  );
  if (useAwait) {
    await call.submit<DelayRes>();
  } else {
    await call.submit<DelayRes>();
  }
  evidence.add(
    `actor-push-await-resumed|rid=${evidence.rid}|spot=${target.spotId}`
    + `|actor=${actor.actorId}|mailbox=${mailboxId}|request=${request.requestId}|handler=actor`
  );
  actor.context.boundSession
    .send(new ActorPushNotify(
      actor.actorId,
      request.requestId,
      request.value,
      String(target.nodeRid)
    ))
    .submit();
  evidence.add(
    `actor-push-await-completed|rid=${evidence.rid}|spot=${target.spotId}`
    + `|actor=${actor.actorId}|mailbox=${mailboxId}|request=${request.requestId}|handler=actor`
  );
}

function recordActorFastEvidence(
  evidence: EvidenceStore,
  target: ActorEvidenceTarget,
  actor: AwaitActor,
  request: ActorFastReq | ActorFastMsg
): void {
  const mailboxId = `actor:${actor.actorId}`;
  evidence.add(
    `actor-fast-started|rid=${evidence.rid}|spot=${target.spotId}`
    + `|actor=${actor.actorId}|mailbox=${mailboxId}|request=${request.requestId}`
    + `|marker=${request.marker}|handler=actor`
  );
  evidence.add(
    `actor-fast-completed|rid=${evidence.rid}|spot=${target.spotId}`
    + `|actor=${actor.actorId}|mailbox=${mailboxId}|request=${request.requestId}`
    + `|marker=${request.marker}|handler=actor`
  );
}

function actorReply(
  scenarioId: string,
  requestId: string,
  actor: AwaitActor,
  target: ActorEvidenceTarget,
  marker: string
): ActorAwaitRes {
  return {
    scenarioId,
    requestId,
    actorId: actor.actorId,
    spotId: String(target.spotId),
    nodeRid: String(target.nodeRid),
    marker
  };
}
