import { Injectable } from '@nestjs/common';
import type { ZLinkMessageContext, ZLinkSpotPacketHandler, ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import { ZLinkPacket } from '@zlink-systems/framework';
import type { SlowSpotRes, SlowSpotReq, StateMsg, StateRes, StateReq } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import type { ScenarioUserSpot } from '../Spots/scenario-spots';

@Injectable()
@ZLinkPacket('StateReq')
export class StateReqHandler implements ZLinkSpotRequestHandler<ScenarioUserSpot, StateReq, StateRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(
    spot: ScenarioUserSpot,
    request: StateReq,
    context: ZLinkMessageContext
  ): Promise<StateRes> {
    void context;
    const delta = request.operation === 'add' ? request.delta : 0;
    const value = spot.add(delta);
    this.evidence.add(`spot-state-request|rid=${this.evidence.rid}|spot=${spot.context.spotId}|value=${value}`);
    return {
      spotId: String(spot.context.spotId),
      nodeRid: String(spot.context.nodeRid),
      value
    };
  }
}

@Injectable()
@ZLinkPacket('StateMsg')
export class StateCommandHandler implements ZLinkSpotPacketHandler<ScenarioUserSpot, StateMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(
    spot: ScenarioUserSpot,
    message: StateMsg,
    context: ZLinkMessageContext
  ): Promise<void> {
    void context;
    this.evidence.add(
      `spot-state-command|rid=${this.evidence.rid}|spot=${spot.context.spotId}|marker=${message.marker}`
    );
  }
}

@Injectable()
@ZLinkPacket('SlowSpotReq')
export class SlowSpotHandler implements ZLinkSpotRequestHandler<ScenarioUserSpot, SlowSpotReq, SlowSpotRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(
    spot: ScenarioUserSpot,
    request: SlowSpotReq,
    context: ZLinkMessageContext
  ): Promise<SlowSpotRes> {
    void context;
    await new Promise((resolve) => setTimeout(resolve, request.delayMs));
    this.evidence.add(
      `slow-spot-request|rid=${this.evidence.rid}|spot=${spot.context.spotId}|marker=${request.marker}`
    );
    return {
      spotId: String(spot.context.spotId),
      nodeRid: String(spot.context.nodeRid),
      marker: request.marker
    };
  }
}
