import { Inject, Injectable } from '@nestjs/common';
import { ZLinkPacket, ZLinkTimerOverrunPolicy, type ZLinkMessageContext, type ZLinkSpotPacketHandler, type ZLinkSpotTimerHandler, type ZLinkTimerTick } from '@zlink-systems/framework';
import { DelayReq, type DelayRes, type TimerStartMsg, type TimerStopMsg } from '../../../Shared/messages';
import { AutomaticTurnDispatchNames } from '../../../Shared/messages';
import { EvidenceStore } from '../Support/evidence-store';
import type { AwaitProbeSpot } from '../Spots/await-probe-spot';
import { AwaitTimerState } from '../Spots/await-timer-state';
import { ZLINK_ROUTE_CLIENT } from '@zlink-systems/nestjs';
import type { ZLinkRouteClient } from '@zlink-systems/framework';

@Injectable()
@ZLinkPacket('TimerStartMsg')
export class TimerStartCommandHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, TimerStartMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: AwaitProbeSpot, request: TimerStartMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    const state = new AwaitTimerState(request.requestId, request.timerName, request.mode, request.delayMs);
    if (!spot.tryAddTimerState(state)) {
      this.evidence.add(
        `timer-start-duplicate-ignored|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${request.requestId}|timer=${request.timerName}|mode=${request.mode}`
      );
      return;
    }

    state.timer = await spot.context.addTimer(
      request.timerName,
      request.periodMs,
      AwaitTimerHandler,
      { overrunPolicy: ZLinkTimerOverrunPolicy.DelayNextTick }
    );
    this.evidence.add(
      `timer-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
      + `|request=${request.requestId}|timer=${request.timerName}|mode=${request.mode}`
    );
  }
}

@Injectable()
@ZLinkPacket('TimerStopMsg')
export class TimerStopCommandHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, TimerStopMsg> {
  async handle(spot: AwaitProbeSpot, request: TimerStopMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    spot.stopScenarioTimers(request.requestId);
  }
}

@Injectable()
export class AwaitTimerHandler implements ZLinkSpotTimerHandler<AwaitProbeSpot> {
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly route: ZLinkRouteClient
  ) {}

  async handle(spot: AwaitProbeSpot, tick: ZLinkTimerTick): Promise<void> {
    const state = spot.findTimerState(tick.name);
    if (state === undefined) {
      return;
    }
    const tickNumber = state.nextTick();
    if (state.mode === 'fast') {
      this.evidence.add(
        `timer-fast-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${state.requestId}|timer=${state.timerName}|tick=${tickNumber}|handler=timer`
      );
      this.evidence.add(
        `timer-fast-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${state.requestId}|timer=${state.timerName}|tick=${tickNumber}|handler=timer`
      );
      return;
    }

    if (tickNumber === 1 && (state.mode === 'yield-on-first' || state.mode === 'yield-then-next')) {
      this.evidence.add(
        `timer-yield-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${state.requestId}|timer=${state.timerName}|tick=${tickNumber}|handler=timer`
      );
      const call = this.route
        .requestToChannel(AutomaticTurnDispatchNames.delayChannel,
          new DelayReq(state.requestId, state.delayMs, state.timerName))
        .timeout(5000);
      this.evidence.add(
        `timer-yield-released|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${state.requestId}|timer=${state.timerName}|tick=${tickNumber}|handler=timer`
      );
      await call.yield<DelayRes>();
      this.evidence.add(
        `timer-yield-resumed|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${state.requestId}|timer=${state.timerName}|tick=${tickNumber}|handler=timer`
      );
      this.evidence.add(
        `timer-yield-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${state.requestId}|timer=${state.timerName}|tick=${tickNumber}|handler=timer`
      );
      return;
    }

    if (state.mode === 'yield-then-next' && tickNumber === 2) {
      this.evidence.add(
        `timer-next-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${state.requestId}|timer=${state.timerName}|tick=${tickNumber}|handler=timer`
      );
      this.evidence.add(
        `timer-next-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${state.requestId}|timer=${state.timerName}|tick=${tickNumber}|handler=timer`
      );
    }
  }
}
