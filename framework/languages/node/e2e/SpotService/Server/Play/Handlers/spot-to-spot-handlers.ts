import { Injectable } from '@nestjs/common';
import type { ZLinkMessageContext, ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import { ZLinkPacket } from '@zlink-systems/framework';
import type {
  SlowSpotRes,
  SpotToSpotNegativeRes,
  SpotToSpotNegativeReq,
  SpotToSpotRes,
  SpotToSpotReq,
  SpotToSpotTimeoutRes,
  SpotToSpotTimeoutReq,
  StateRes
} from '../../../Shared/messages';
import {
  MissingSpotMsg,
  MissingSpotReq,
  SlowSpotReq,
  SpotMsg,
  SpotServiceNames,
  StateMsg,
  StateReq,
  spotServicePacket
} from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import type { ScenarioUserSpot } from '../Spots/scenario-spots';

@Injectable()
@ZLinkPacket('SpotToSpotReq')
export class SpotToSpotHandler implements ZLinkSpotRequestHandler<ScenarioUserSpot, SpotToSpotReq, SpotToSpotRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(
    spot: ScenarioUserSpot,
    request: SpotToSpotReq,
    context: ZLinkMessageContext
  ): Promise<SpotToSpotRes> {
    void context;
    const reply = await spot.context.outbound
      .requestToSpot(request.targetSpotId, spotServicePacket(StateReq, { operation: 'add', delta: 3 }))
      .submit<StateRes>();
    await spot.context.outbound
      .sendToSpot(request.targetSpotId,
        spotServicePacket(StateMsg, { marker: `sm-c3-send-${request.marker}` }))
      .submit();
    await spot.context.outbound
      .publish(SpotServiceNames.spotChannel, SpotServiceNames.spotEventTopic,
        spotServicePacket(SpotMsg, { marker: `sm-c3-publish-${request.marker}` }))
      .submit();
    this.evidence.add(
      `spot-to-spot|rid=${this.evidence.rid}|source=${spot.context.spotId}`
      + `|target=${request.targetSpotId}|value=${reply.value}`
    );
    return {
      sourceSpotId: String(spot.context.spotId),
      targetSpotId: request.targetSpotId,
      targetValue: reply.value
    };
  }
}

@Injectable()
@ZLinkPacket('SpotToSpotTimeoutReq')
export class SpotToSpotTimeoutHandler
  implements ZLinkSpotRequestHandler<ScenarioUserSpot, SpotToSpotTimeoutReq, SpotToSpotTimeoutRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(
    spot: ScenarioUserSpot,
    request: SpotToSpotTimeoutReq,
    context: ZLinkMessageContext
  ): Promise<SpotToSpotTimeoutRes> {
    void context;
    let failed = false;
    try {
      await spot.context.outbound
        .requestToSpot(request.targetSpotId,
          spotServicePacket(SlowSpotReq, { marker: request.marker, delayMs: 1500 }))
        .timeout(100)
        .submit<SlowSpotRes>();
    } catch {
      failed = true;
    }
    this.evidence.add(
      `spot-to-spot-timeout|rid=${this.evidence.rid}|source=${spot.context.spotId}`
      + `|target=${request.targetSpotId}|failed=${failed ? 'True' : 'False'}`
    );
    return {
      sourceSpotId: String(spot.context.spotId),
      targetSpotId: request.targetSpotId,
      failed
    };
  }
}

@Injectable()
@ZLinkPacket('SpotToSpotNegativeReq')
export class SpotToSpotNegativeHandler
  implements ZLinkSpotRequestHandler<ScenarioUserSpot, SpotToSpotNegativeReq, SpotToSpotNegativeRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(
    spot: ScenarioUserSpot,
    request: SpotToSpotNegativeReq,
    context: ZLinkMessageContext
  ): Promise<SpotToSpotNegativeRes> {
    void context;
    let requestFailed = false;
    try {
      await spot.context.outbound
        .requestToSpot(request.targetSpotId,
          spotServicePacket(MissingSpotReq, { operation: 'noop', delta: 0 }))
        .timeout(2000)
        .submit<StateRes>();
    } catch {
      requestFailed = true;
    }
    await spot.context.outbound
      .sendToSpot(request.targetSpotId,
        spotServicePacket(MissingSpotMsg, { marker: `missing-${request.marker}` }))
      .submit();
    this.evidence.add(
      `spot-to-spot-negative|rid=${this.evidence.rid}|source=${spot.context.spotId}`
      + `|target=${request.targetSpotId}|requestFailed=${requestFailed ? 'True' : 'False'}`
    );
    return {
      sourceSpotId: String(spot.context.spotId),
      targetSpotId: request.targetSpotId,
      requestFailed
    };
  }
}
