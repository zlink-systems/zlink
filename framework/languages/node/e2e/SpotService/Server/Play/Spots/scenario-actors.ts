import type {
  ActorPushReq,
  ComplexActorReq,
  ComplexActorRes,
  ActorPingReq,
  ActorPingRes,
  DestroyActorRes,
  DestroyActorReq,
  EnsureActorReq,
  JoinUserSpotActorReq,
  JoinUserSpotActorRes,
  LeaveRes,
  LeaveReq,
  SlowActorPingReq,
  SnapshotRes,
  SnapshotReq
} from '../../../Shared/messages';
import { ActorPushNotify } from '../../../Shared/messages';
import type {
  ZLinkActor,
  ZLinkActorCreateResponse,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkEntrySpot,
  ZLinkEntrySpotActorSendHandler,
  ZLinkEntrySpotContext,
  ZLinkMessage,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import { ZLinkSpotActorRequest, ZLinkSpotActorSend } from '@zlink-systems/framework';
import {
  zlinkEntrySpotActorRequestHandler,
  zlinkEntrySpotActorSendHandler
} from '@zlink-systems/nestjs';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import { InMemoryActorSpotStore } from '../Infrastructure/actor-spot-store';

export class ScenarioActor implements ZLinkActor {
  displayName: string;
  seen = 0;

  constructor(readonly actorId: string, readonly context: ZLinkActorContext) {
    this.displayName = actorId;
  }
}

class InitializeScenarioActor {
  constructor(readonly displayName: string) {}
}

@zlinkEntrySpotActorSendHandler({
  actor: () => ScenarioActor,
  entrySpot: () => ScenarioEntrySpot,
  packetName: 'InitializeScenarioActor'
})
export class InitializeScenarioActorHandler
  implements ZLinkEntrySpotActorSendHandler<ScenarioEntrySpot, ScenarioActor, InitializeScenarioActor> {
  @ZLinkSpotActorSend('InitializeScenarioActor')
  async handle(
    _spot: ZLinkEntrySpot<ScenarioActor>,
    actor: ScenarioActor,
    _context: ZLinkMessageContext,
    message: InitializeScenarioActor
  ): Promise<void> {
    actor.displayName = message.displayName;
  }
}

export class ScenarioActorFactory implements ZLinkActorFactory {
  async create(context: ZLinkActorContext): Promise<ScenarioActor> {
    return new ScenarioActor(context.actorId, context);
  }
}

export class ScenarioEntrySpot implements ZLinkEntrySpot<ScenarioActor> {
  private static evidence?: EvidenceStore;
  readonly context!: ZLinkEntrySpotContext<ScenarioActor>;

  static useEvidence(evidence: EvidenceStore): void {
    this.evidence = evidence;
  }

  async onCreateActor(actor: ScenarioActor, createRequest: ZLinkMessage): Promise<ZLinkActorCreateResponse> {
    const request = createRequest.decode<Partial<EnsureActorReq>>(Object as never);
    if (typeof request.displayName === 'string') {
      actor.displayName = request.displayName;
    }
    const evidence = ScenarioEntrySpot.requireEvidence();
    evidence.add(`entry-created|rid=${evidence.rid}|actor=${actor.actorId}`);
    return { accepted: true };
  }

  async onJoinedActor(actor: ScenarioActor): Promise<void> {
    const evidence = ScenarioEntrySpot.requireEvidence();
    evidence.add(`entry-joined|rid=${evidence.rid}|actor=${actor.actorId}`);
  }

  async onLeaveActor(actor: ScenarioActor): Promise<void> {
    const evidence = ScenarioEntrySpot.requireEvidence();
    evidence.add(`entry-left|rid=${evidence.rid}|actor=${actor.actorId}`);
  }

  async onDisconnectActor(actor: ScenarioActor): Promise<void> {
    const evidence = ScenarioEntrySpot.requireEvidence();
    evidence.add(`entry-disconnected|rid=${evidence.rid}|actor=${actor.actorId}`);
    if (actor.actorId.startsWith('actor-sm-d5-fail-')) {
      throw new Error('SM-D5 injected disconnect callback failure.');
    }
  }

  scheduleDestroy(actor: ScenarioActor): void {
    const evidence = ScenarioEntrySpot.requireEvidence();
    void this.context.runIoWorker(async () => true).submit().then(async () => {
      try {
        await this.context.destroyActor(actor);
        evidence.add(`actor-destroyed|rid=${evidence.rid}|actor=${actor.actorId}`);
      } catch (error) {
        evidence.add(
          `actor-destroy-failed|rid=${evidence.rid}|actor=${actor.actorId}`
          + `|error=${error instanceof Error ? error.name : String(error)}`
        );
      }
    });
  }

  addEvidence(entry: string): void {
    ScenarioEntrySpot.requireEvidence().add(entry);
  }

  private static requireEvidence(): EvidenceStore {
    if (this.evidence === undefined) {
      throw new Error('ScenarioEntrySpot evidence store is not configured.');
    }
    return this.evidence;
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => ScenarioActor,
  entrySpot: () => ScenarioEntrySpot,
  packetName: 'ActorPingReq'
})
export class EntryActorPingHandler {
  private static evidence?: EvidenceStore;

  static useEvidence(evidence: EvidenceStore): void {
    this.evidence = evidence;
  }

  @ZLinkSpotActorRequest('ActorPingReq')
  async handle(
    _spot: ScenarioEntrySpot,
    actor: ScenarioActor,
    context: ZLinkMessageContext,
    request: ActorPingReq
  ): Promise<ActorPingRes> {
    void context;
    const evidence = EntryActorPingHandler.requireEvidence();
    actor.seen += 1;
    evidence.add(
      `actor-pingMsg|rid=${evidence.rid}|actor=${actor.actorId}`
      + `|spot=${actor.context.spotId ?? evidence.rid}|value=${request.value}|seen=${actor.seen}`
    );
    return {
      actorId: actor.actorId,
      nodeRid: evidence.rid,
      spotId: String(actor.context.spotId ?? evidence.rid),
      value: request.value,
      seen: actor.seen
    };
  }

  private static requireEvidence(): EvidenceStore {
    if (this.evidence === undefined) {
      throw new Error('EntryActorPingHandler evidence store is not configured.');
    }
    return this.evidence;
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => ScenarioActor,
  entrySpot: () => ScenarioEntrySpot,
  packetName: 'SlowActorPingReq'
})
export class EntrySlowActorPingHandler {
  private static evidence?: EvidenceStore;

  static useEvidence(evidence: EvidenceStore): void {
    this.evidence = evidence;
  }

  @ZLinkSpotActorRequest('SlowActorPingReq')
  async handle(
    _spot: ScenarioEntrySpot,
    actor: ScenarioActor,
    context: ZLinkMessageContext,
    request: SlowActorPingReq
  ): Promise<ActorPingRes> {
    void context;
    const evidence = EntrySlowActorPingHandler.requireEvidence();
    evidence.add(
      `actor-slow-ping-start|rid=${evidence.rid}|actor=${actor.actorId}`
      + `|spot=${actor.context.spotId ?? evidence.rid}|value=${request.value}`
    );
    await new Promise((resolve) => setTimeout(resolve, Math.max(0, request.delayMs)));
    actor.seen += 1;
    evidence.add(
      `actor-slow-pingMsg|rid=${evidence.rid}|actor=${actor.actorId}`
      + `|spot=${actor.context.spotId ?? evidence.rid}|value=${request.value}|seen=${actor.seen}`
    );
    return {
      actorId: actor.actorId,
      nodeRid: evidence.rid,
      spotId: String(actor.context.spotId ?? evidence.rid),
      value: request.value,
      seen: actor.seen
    };
  }

  private static requireEvidence(): EvidenceStore {
    if (this.evidence === undefined) {
      throw new Error('EntrySlowActorPingHandler evidence store is not configured.');
    }
    return this.evidence;
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => ScenarioActor,
  entrySpot: () => ScenarioEntrySpot,
  packetName: 'UserActorPingReq'
})
export class EntryUserActorPingHandler {
  private static evidence?: EvidenceStore;

  static useEvidence(evidence: EvidenceStore): void {
    this.evidence = evidence;
  }

  @ZLinkSpotActorRequest('UserActorPingReq')
  async handle(
    _spot: ScenarioEntrySpot,
    actor: ScenarioActor,
    context: ZLinkMessageContext,
    request: ActorPingReq
  ): Promise<ActorPingRes> {
    void context;
    const evidence = EntryUserActorPingHandler.requireEvidence();
    actor.seen += 1;
    const spotId = InMemoryActorSpotStore.find(actor.actorId) ?? actor.displayName;
    evidence.add(
      `actor-pingMsg|rid=${evidence.rid}|actor=${actor.actorId}`
      + `|spot=${spotId}|value=${request.value}|seen=${actor.seen}`
    );
    return {
      actorId: actor.actorId,
      nodeRid: evidence.rid,
      spotId,
      value: request.value,
      seen: actor.seen
    };
  }

  private static requireEvidence(): EvidenceStore {
    if (this.evidence === undefined) {
      throw new Error('EntryUserActorPingHandler evidence store is not configured.');
    }
    return this.evidence;
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => ScenarioActor,
  entrySpot: () => ScenarioEntrySpot,
  packetName: 'ActorPushReq'
})
export class ActorPushHandler {
  constructor(private readonly evidence: EvidenceStore) {}

  @ZLinkSpotActorRequest('ActorPushReq')
  async handle(
    _spot: ScenarioEntrySpot,
    actor: ScenarioActor,
    context: ZLinkMessageContext,
    request: ActorPushReq
  ): Promise<ActorPingRes> {
    void context;
    actor.seen += 1;
    actor.context.boundSession
      .send(new ActorPushNotify(actor.actorId, request.value, actor.seen))
      .submit();
    return {
      actorId: actor.actorId,
      nodeRid: this.evidence.rid,
      spotId: String(actor.context.spotId ?? this.evidence.rid),
      value: request.value,
      seen: actor.seen
    };
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => ScenarioActor,
  entrySpot: () => ScenarioEntrySpot,
  packetName: 'UserActorPushReq'
})
export class EntryUserActorPushHandler {
  constructor(private readonly evidence: EvidenceStore) {}

  @ZLinkSpotActorRequest('UserActorPushReq')
  async handle(
    _spot: ScenarioEntrySpot,
    actor: ScenarioActor,
    context: ZLinkMessageContext,
    request: ActorPushReq
  ): Promise<ActorPingRes> {
    void context;
    actor.seen += 1;
    const spotId = InMemoryActorSpotStore.find(actor.actorId) ?? actor.displayName;
    actor.context.boundSession
      .send(new ActorPushNotify(actor.actorId, request.value, actor.seen))
      .submit();
    return {
      actorId: actor.actorId,
      nodeRid: this.evidence.rid,
      spotId,
      value: request.value,
      seen: actor.seen
    };
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => ScenarioActor,
  entrySpot: () => ScenarioEntrySpot,
  packetName: 'ComplexActorReq'
})
export class ComplexActorHandler {
  private static evidence?: EvidenceStore;

  static useEvidence(evidence: EvidenceStore): void {
    this.evidence = evidence;
  }

  @ZLinkSpotActorRequest('ComplexActorReq')
  async handle(
    _spot: ScenarioEntrySpot,
    actor: ScenarioActor,
    context: ZLinkMessageContext,
    request: ComplexActorReq
  ): Promise<ComplexActorRes> {
    void context;
    const evidence = ComplexActorHandler.requireEvidence();
    actor.displayName = request.displayName;
    const attrs = Object.entries(request.attributes)
      .sort(([left], [right]) => left.localeCompare(right))
      .map(([key, value]) => `${key}:${value}`)
      .join(',');
    evidence.add(
      `actor-complex|rid=${evidence.rid}|actor=${actor.actorId}|name=${request.displayName}`
      + `|level=${request.level}|tags=${request.tags.join(',')}|attrs=${attrs}`
    );
    return {
      actorId: actor.actorId,
      displayName: request.displayName,
      level: request.level,
      tags: request.tags,
      attributes: request.attributes
    };
  }

  private static requireEvidence(): EvidenceStore {
    if (this.evidence === undefined) {
      throw new Error('ComplexActorHandler evidence store is not configured.');
    }
    return this.evidence;
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => ScenarioActor,
  entrySpot: () => ScenarioEntrySpot,
  packetName: 'LeaveReq'
})
export class EntryActorLeaveHandler {
  private static evidence?: EvidenceStore;

  static useEvidence(evidence: EvidenceStore): void {
    this.evidence = evidence;
  }

  @ZLinkSpotActorRequest('LeaveReq')
  async handle(
    _spot: ScenarioEntrySpot,
    actor: ScenarioActor,
    context: ZLinkMessageContext,
    request: LeaveReq
  ): Promise<LeaveRes> {
    void context;
    if (request.actorId !== actor.actorId) {
      throw new Error('Leave request actor does not match dispatched actor.');
    }
    const evidence = EntryActorLeaveHandler.requireEvidence();
    const spotId = InMemoryActorSpotStore.find(actor.actorId) ?? actor.displayName;
    evidence.add(
      `spot-actor-left|rid=${evidence.rid}|spot=${spotId}|actor=${actor.actorId}`
    );
    actor.context.joinEntrySpot(request).timeout(5000).defer();
    return {
      actorId: actor.actorId,
      accepted: true
    };
  }

  private static requireEvidence(): EvidenceStore {
    if (this.evidence === undefined) {
      throw new Error('EntryActorLeaveHandler evidence store is not configured.');
    }
    return this.evidence;
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => ScenarioActor,
  entrySpot: () => ScenarioEntrySpot,
  packetName: 'JoinUserSpotActorReq'
})
export class EntryUserSpotActorJoinHandler {
  @ZLinkSpotActorRequest('JoinUserSpotActorReq')
  async handle(
    _spot: ScenarioEntrySpot,
    actor: ScenarioActor,
    context: ZLinkMessageContext,
    request: JoinUserSpotActorReq
  ): Promise<JoinUserSpotActorRes> {
    if (request.actorId !== actor.actorId) {
      throw new Error('Join request actor does not match dispatched actor.');
    }
    actor.context
      .joinSpot(request.spotId, request)
      .timeout(5000)
      .defer();
    InMemoryActorSpotStore.record(actor.actorId, request.spotId);
    return {
      spotId: request.spotId,
      actorId: actor.actorId,
      accepted: true,
      generation: actor.context.objectGeneration.toString()
    };
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => ScenarioActor,
  entrySpot: () => ScenarioEntrySpot,
  packetName: 'SnapshotReq'
})
export class EntryActorSnapshotHandler {
  @ZLinkSpotActorRequest('SnapshotReq')
  async handle(
    _spot: ScenarioEntrySpot,
    actor: ScenarioActor,
    context: ZLinkMessageContext,
    request: SnapshotReq
  ): Promise<SnapshotRes> {
    void context;
    if (request.actorId !== actor.actorId) {
      throw new Error('Snapshot request actor does not match dispatched actor.');
    }
    return {
      actorId: actor.actorId,
      seen: actor.seen
    };
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => ScenarioActor,
  entrySpot: () => ScenarioEntrySpot,
  packetName: 'DestroyActorReq'
})
export class EntryActorDestroyHandler
{
  async handle(
    spot: ScenarioEntrySpot,
    actor: ScenarioActor,
    context: ZLinkMessageContext,
    request: DestroyActorReq
  ): Promise<DestroyActorRes> {
    void context;
    if (request.actorId !== actor.actorId) {
      throw new Error('Destroy request actor does not match dispatched actor.');
    }
    spot.scheduleDestroy(actor);
    return {
      actorId: actor.actorId,
      destroyed: true
    };
  }
}
