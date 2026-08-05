import { Inject, Injectable } from '@nestjs/common';
import { ZLinkPacket, type ZLinkMessageContext, type ZLinkSpotPacketHandler, type ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import { DelayReq } from '../../../Shared/messages';
import type {
  DelayRes,
  HoldMsg,
  ProbeMsg,
  WorkerAwaitMsg,
  AwaitMsg,
  AwaitReq,
  ProbeReq,
  AutomaticTurnDispatchRes
} from '../../../Shared/messages';
import { AutomaticTurnDispatchNames } from '../../../Shared/messages';
import { EvidenceStore } from '../Support/evidence-store';
import { ZLINK_ROUTE_CLIENT } from '@zlink-systems/nestjs';
import type { ZLinkRouteClient } from '@zlink-systems/framework';
import type { AwaitProbeSpot } from '../Spots/await-probe-spot';

@Injectable()
@ZLinkPacket('HoldMsg')
export class HoldCommandHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, HoldMsg> {
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly route: ZLinkRouteClient
  ) {}

  async handle(spot: AwaitProbeSpot, request: HoldMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    this.evidence.add(`hold-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}|handler=spot`);
    await this.route
      .requestToChannel(AutomaticTurnDispatchNames.delayChannel,
        new DelayReq(request.requestId, request.delayMs, 'hold'))
      .timeout(5000)
      .submit<DelayRes>();
    this.evidence.add(`hold-resumed|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}|handler=spot`);
    this.evidence.add(`hold-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}|handler=spot`);
  }
}

@Injectable()
@ZLinkPacket('AwaitMsg')
export class AwaitCommandHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, AwaitMsg> {
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly route: ZLinkRouteClient
  ) {}

  async handle(spot: AwaitProbeSpot, request: AwaitMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    const terminator = request.terminator ?? 'async';
    this.evidence.add(
      `${terminator}-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}`
      + `|correlation=${request.correlationId}|handler=spot`
    );
    const call = this.route
      .requestToChannel(AutomaticTurnDispatchNames.delayChannel,
        new DelayReq(request.requestId, request.delayMs, 'await'))
      .timeout(5000);
    this.evidence.add(
      `${terminator}-${terminator === 'yield' ? 'released' : 'held'}|rid=${this.evidence.rid}`
      + `|spot=${spot.context.spotId}|request=${request.requestId}`
      + `|correlation=${request.correlationId}|handler=spot`
    );
    if (terminator === 'yield') {
      await call.yield<DelayRes>();
    } else {
      await call.submit<DelayRes>();
    }
    this.evidence.add(
      `${terminator}-resumed|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}`
      + `|correlation=${request.correlationId}|handler=spot`
    );
    this.evidence.add(
      `${terminator}-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}`
      + `|correlation=${request.correlationId}|handler=spot`
    );
  }
}

@Injectable()
@ZLinkPacket('AwaitReq')
export class AwaitRequestHandler implements ZLinkSpotRequestHandler<AwaitProbeSpot, AwaitReq, AutomaticTurnDispatchRes> {
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly route: ZLinkRouteClient
  ) {}

  async handle(
    spot: AwaitProbeSpot,
    request: AwaitReq,
    context: ZLinkMessageContext
  ): Promise<AutomaticTurnDispatchRes> {
    await new AwaitCommandHandler(this.evidence, this.route).handle(spot, request, context);
    return {
      scenarioId: request.correlationId,
      requestId: request.requestId,
      spotId: String(spot.context.spotId),
      nodeRid: String(spot.context.nodeRid),
      marker: `${request.terminator ?? 'async'}-completed`
    };
  }
}

@Injectable()
@ZLinkPacket('WorkerAwaitMsg')
export class WorkerAwaitCommandHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, WorkerAwaitMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: AwaitProbeSpot, request: WorkerAwaitMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    this.evidence.add(`worker-await-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}|handler=spot`);
    const call = spot.context.runIoWorker(async (signal) => {
      await delay(request.delayMs, signal);
      return request.requestId;
    });
    this.evidence.add(`worker-await-released|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}|handler=spot`);
    await call.yield();
    this.evidence.add(`worker-await-resumed|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}|handler=spot`);
    this.evidence.add(`worker-await-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}|handler=spot`);
  }
}

@Injectable()
@ZLinkPacket('ProbeMsg')
export class ProbeCommandHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, ProbeMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: AwaitProbeSpot, request: ProbeMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    this.evidence.add(
      `probe-started|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}`
      + `|marker=${request.marker}|handler=spot`
    );
    this.evidence.add(
      `probe-completed|rid=${this.evidence.rid}|spot=${spot.context.spotId}|request=${request.requestId}`
      + `|marker=${request.marker}|handler=spot`
    );
  }
}

@Injectable()
@ZLinkPacket('ProbeReq')
export class ProbeRequestHandler implements ZLinkSpotRequestHandler<AwaitProbeSpot, ProbeReq, AutomaticTurnDispatchRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: AwaitProbeSpot, request: ProbeReq, context: ZLinkMessageContext): Promise<AutomaticTurnDispatchRes> {
    await new ProbeCommandHandler(this.evidence).handle(spot, request, context);
    return {
      scenarioId: request.requestId.split('-', 1)[0] ?? 'TD',
      requestId: request.requestId,
      spotId: String(spot.context.spotId),
      nodeRid: String(spot.context.nodeRid),
      marker: request.marker
    };
  }
}

function delay(delayMs: number, signal: AbortSignal): Promise<void> {
  return new Promise<void>((resolve, reject) => {
    if (signal.aborted) {
      reject(signal.reason instanceof Error ? signal.reason : new Error('Worker await was aborted.'));
      return;
    }
    const timer = setTimeout(resolve, delayMs);
    signal.addEventListener('abort', () => {
      clearTimeout(timer);
      reject(signal.reason instanceof Error ? signal.reason : new Error('Worker await was aborted.'));
    }, { once: true });
  });
}
