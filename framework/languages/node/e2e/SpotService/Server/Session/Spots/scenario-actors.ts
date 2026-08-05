import type {
  ZLinkActor,
  ZLinkActorCreateResponse,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage
} from '@zlink-systems/framework';
import { EvidenceStore } from '../Infrastructure/evidence-store';

export class ScenarioActor implements ZLinkActor {
  displayName: string;

  constructor(readonly actorId: string, readonly context: ZLinkActorContext) {
    this.displayName = actorId;
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
    void createRequest;
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

  private static requireEvidence(): EvidenceStore {
    if (this.evidence === undefined) {
      throw new Error('ScenarioEntrySpot evidence store is not configured.');
    }
    return this.evidence;
  }
}
