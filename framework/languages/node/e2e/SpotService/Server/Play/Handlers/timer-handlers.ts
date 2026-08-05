import { Injectable } from '@nestjs/common';
import type { ZLinkSpotTimerHandler, ZLinkTimerTick } from '@zlink-systems/framework';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import type { ScenarioUserSpot } from '../Spots/scenario-spots';

@Injectable()
export class BasicTimerHandler implements ZLinkSpotTimerHandler<ScenarioUserSpot> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: ScenarioUserSpot, tick: ZLinkTimerTick): Promise<void> {
    this.evidence.add(`timer-basic|rid=${this.evidence.rid}|spot=${spot.context.spotId}|name=${tick.name}`);
  }
}

@Injectable()
export class IdleCloseTimerHandler implements ZLinkSpotTimerHandler<ScenarioUserSpot> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: ScenarioUserSpot, tick: ZLinkTimerTick): Promise<void> {
    if (tick.deliveryIndex > 1) {
      return;
    }
    const closed = await spot.context.close();
    this.evidence.add(
      `timer-idle-close|rid=${this.evidence.rid}|spot=${spot.context.spotId}|name=${tick.name}|closed=${closed ? 'True' : 'False'}`
    );
  }
}

@Injectable()
export class OverrunTimerHandler implements ZLinkSpotTimerHandler<ScenarioUserSpot> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: ScenarioUserSpot, tick: ZLinkTimerTick): Promise<void> {
    this.evidence.add(
      `timer-overrun|rid=${this.evidence.rid}|spot=${spot.context.spotId}|name=${tick.name}`
      + `|delivery=${tick.deliveryIndex}|scheduled=${tick.scheduledIndex}|skipped=${tick.skippedTicks}`
    );
    await new Promise((resolve) => setTimeout(resolve, 90));
  }
}
