import { Inject, Injectable } from '@nestjs/common';
import { ZLinkMessage, ZLinkUserSpotExecutionMode } from '@zlink-systems/framework';
import type { ZLinkActorManager, ZLinkMessageContext, ZLinkRequestHandler, ZLinkSpotManager } from '@zlink-systems/framework';
import { ZLINK_ACTOR_MANAGER, ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import type {
  BindAwaitActorsRes,
  BindAwaitActorsReq,
  EnsureSpotRes,
  EnsureSpotReq,
  AwaitEvidenceRes,
  AwaitEvidenceReq,
  AwaitEvidenceWaitReq
} from '../../../Shared/messages';
import { AutomaticTurnDispatchNames } from '../../../Shared/messages';
import { EvidenceStore } from '../Support/evidence-store';
import { AwaitProbeSpot } from '../Spots/await-probe-spot';

export const YIELD_PLAY_NODE_RID = 'YIELD_PLAY_NODE_RID';

@Injectable()
export class EnsureSpotControlHandler implements ZLinkRequestHandler<EnsureSpotReq, EnsureSpotRes> {
  constructor(
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager,
    @Inject(YIELD_PLAY_NODE_RID) private readonly nodeRid: string
  ) {}

  async handle(request: EnsureSpotReq, context: ZLinkMessageContext): Promise<EnsureSpotRes> {
    void context;
    const created = await this.spots.getOrCreate(
      request.spotId,
      request.executionMode === ZLinkUserSpotExecutionMode.PerActor
        ? AutomaticTurnDispatchNames.perActorSpotType
        : AwaitProbeSpot.name
    )
      .inMesh(AutomaticTurnDispatchNames.spotChannel)
      .submit();
    return {
      spotId: String(created.spot.spotId),
      nodeRid: String(created.spot.nodeRid ?? this.nodeRid)
    };
  }
}

@Injectable()
export class BindAwaitActorsControlHandler implements ZLinkRequestHandler<BindAwaitActorsReq, BindAwaitActorsRes> {
  constructor(@Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager) {}

  async handle(request: BindAwaitActorsReq, context: ZLinkMessageContext): Promise<BindAwaitActorsRes> {
    void context;
    const actors = await Promise.all(request.actorIds.map(async (actorId) => {
      const created = await this.actors.getOrCreate(
        actorId,
        AutomaticTurnDispatchNames.actorType
      )
        .inMesh(AutomaticTurnDispatchNames.spotChannel)
        .request(ZLinkMessage.from({ spotId: request.spotId }))
        .submit();
      if (created.status === 'rejected') {
        throw new Error(`Actor '${actorId}' creation was rejected.`);
      }
      const actor = created.actor;
      return {
        actorId: actor.actorId,
        nodeRid: String(actor.nodeRid),
        generation: actor.objectGeneration.toString()
      };
    }));
    return {
      spotId: request.spotId,
      actors
    };
  }
}

@Injectable()
export class AwaitEvidenceControlHandler implements ZLinkRequestHandler<AwaitEvidenceReq, AwaitEvidenceRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: AwaitEvidenceReq, context: ZLinkMessageContext): Promise<AwaitEvidenceRes> {
    void context;
    return {
      requestId: request.requestId,
      evidence: this.evidence.snapshot()
    };
  }
}

@Injectable()
export class AwaitEvidenceWaitControlHandler implements ZLinkRequestHandler<AwaitEvidenceWaitReq, AwaitEvidenceRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: AwaitEvidenceWaitReq, context: ZLinkMessageContext): Promise<AwaitEvidenceRes> {
    void context;
    const timeoutMs = Math.max(1, Math.min(request.timeoutMilliseconds ?? 20000, 30000));
    const snapshot = await this.evidence.waitUntil((entries) =>
      entries.some((entry) => entry.includes(`request=${request.requestId}`) && entry.includes(request.marker)), timeoutMs);
    return {
      requestId: request.requestId,
      evidence: snapshot
    };
  }
}
