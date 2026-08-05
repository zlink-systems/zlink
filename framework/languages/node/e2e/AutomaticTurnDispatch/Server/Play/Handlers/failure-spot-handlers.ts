import { Inject, Injectable } from '@nestjs/common';
import { ZLinkPacket, type ZLinkMessageContext, type ZLinkSpotPacketHandler } from '@zlink-systems/framework';
import {
  DelayReq,
  type DelayRes,
  type AwaitCancelMsg,
  type AwaitTimeoutMsg
} from '../../../Shared/messages';
import { AutomaticTurnDispatchNames } from '../../../Shared/messages';
import { EvidenceStore } from '../Support/evidence-store';
import type { AwaitProbeSpot } from '../Spots/await-probe-spot';
import { ZLINK_ROUTE_CLIENT } from '@zlink-systems/nestjs';
import type { ZLinkRouteClient } from '@zlink-systems/framework';

@Injectable()
@ZLinkPacket('AwaitTimeoutMsg')
export class AwaitTimeoutCommandHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, AwaitTimeoutMsg> {
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly route: ZLinkRouteClient
  ) {}

  async handle(spot: AwaitProbeSpot, request: AwaitTimeoutMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    const terminator = request.terminator ?? 'async';
    this.evidence.add(
      `timeout-await-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
      + `|request=${request.requestId}|handler=spot`
    );
    try {
      const call = this.route
        .requestToChannel(AutomaticTurnDispatchNames.delayChannel,
          new DelayReq(request.requestId, request.delayMs, 'timeout'))
        .timeout(request.timeoutMs);
      this.evidence.add(
        `timeout-${terminator}-${terminator === 'yield' ? 'released' : 'held'}|rid=${this.evidence.rid}`
        + `|spot=${spot.context.spotId}`
        + `|request=${request.requestId}|handler=spot`
      );
      if (terminator === 'yield') await call.yield<DelayRes>();
      else await call.submit<DelayRes>();
      this.evidence.add(
        `timeout-await-unexpected-resumed|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${request.requestId}|handler=spot`
      );
    } catch (error) {
      const errorName = error instanceof Error ? error.name : 'Error';
      this.evidence.add(
        `timeout-await-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${request.requestId}|error=${errorName}|handler=spot`
      );
    }
  }
}

@Injectable()
@ZLinkPacket('AwaitCancelMsg')
export class AwaitCancelCommandHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, AwaitCancelMsg> {
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly route: ZLinkRouteClient
  ) {}

  async handle(spot: AwaitProbeSpot, request: AwaitCancelMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    const terminator = request.terminator ?? 'async';
    this.evidence.add(
      `cancel-await-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
      + `|request=${request.requestId}|handler=spot`
    );
    const controller = new AbortController();
    const cancelTimer = setTimeout(() => controller.abort(), request.cancelAfterMs);
    try {
      const call = this.route
        .requestToChannel(AutomaticTurnDispatchNames.delayChannel,
          new DelayReq(request.requestId, request.delayMs, 'cancel'))
        .timeout(5000);
      this.evidence.add(
        `cancel-${terminator}-${terminator === 'yield' ? 'released' : 'held'}|rid=${this.evidence.rid}`
        + `|spot=${spot.context.spotId}`
        + `|request=${request.requestId}|handler=spot`
      );
      if (terminator === 'yield') await call.yield<DelayRes>(controller.signal);
      else await call.submit<DelayRes>(controller.signal);
      this.evidence.add(
        `cancel-await-unexpected-resumed|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${request.requestId}|handler=spot`
      );
    } catch (error) {
      const errorName = error instanceof Error ? error.name : 'Error';
      this.evidence.add(
        `cancel-await-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
        + `|request=${request.requestId}|error=${errorName}|handler=spot`
      );
    } finally {
      clearTimeout(cancelTimer);
    }
  }
}
