import { Injectable } from '@nestjs/common';
import type {
  ZLinkMessageContext,
  ZLinkSpotPacketHandler,
  ZLinkSpotRequestHandler,
  ZLinkSpotTimerHandler,
  ZLinkTimerTick
} from '@zlink-systems/framework';
import { ZLinkPacket } from '@zlink-systems/framework';
import type { StageProbeReq, StageTimerStartMsg, StateRes } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import type { ScenarioUserSpot } from '../Spots/scenario-spots';

class ScenarioStage {
  constructor(private readonly spot: ScenarioUserSpot) {}

  apply(request: StageProbeReq, evidence: EvidenceStore): StateRes {
    const value = this.spot.add(request.delta);
    evidence.add(
      `stage-request|rid=${evidence.rid}|spot=${this.spot.context.spotId}`
      + `|marker=${request.marker}|value=${value}`
    );
    return {
      spotId: String(this.spot.context.spotId),
      nodeRid: String(this.spot.context.nodeRid),
      value
    };
  }

  async startTimer(command: StageTimerStartMsg): Promise<void> {
    await this.spot.context.addTimer(command.name, command.periodMs, StageTimerHandler);
  }
}

@Injectable()
@ZLinkPacket('StageProbeReq')
export class StageProbeHandler implements ZLinkSpotRequestHandler<ScenarioUserSpot, StageProbeReq, StateRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(
    spot: ScenarioUserSpot,
    request: StageProbeReq,
    context: ZLinkMessageContext
  ): Promise<StateRes> {
    void context;
    return new ScenarioStage(spot).apply(request, this.evidence);
  }
}

@Injectable()
@ZLinkPacket('StageTimerStartMsg')
export class StageTimerStartHandler implements ZLinkSpotPacketHandler<ScenarioUserSpot, StageTimerStartMsg> {
  async handle(
    spot: ScenarioUserSpot,
    message: StageTimerStartMsg,
    context: ZLinkMessageContext
  ): Promise<void> {
    void context;
    await new ScenarioStage(spot).startTimer(message);
  }
}

@Injectable()
export class StageTimerHandler implements ZLinkSpotTimerHandler<ScenarioUserSpot> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: ScenarioUserSpot, tick: ZLinkTimerTick): Promise<void> {
    this.evidence.add(
      `stage-timer|rid=${this.evidence.rid}|spot=${spot.context.spotId}|name=${tick.name}`
      + `|delivery=${tick.deliveryIndex}`
    );
  }
}
