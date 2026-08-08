import {
  ZLinkPacket,
  type ZLinkInstanceSpot,
  type ZLinkInstanceSpotContext,
  type ZLinkSpot,
  type ZLinkSpotTimerHandler,
  type ZLinkTimerTick,
  type ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import { Injectable, Scope } from '@nestjs/common';
import { ObjectReq, type ObjectRes } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import { waitForScenarioGate } from '../Infrastructure/scenario-gates';

let objectEvidence: EvidenceStore | undefined;
let objectActivationDelayMs = 0;

export function configureObjectEvidence(value: EvidenceStore): void {
  objectEvidence = value;
}

export function configureObjectActivationDelay(delayMs: number): void {
  objectActivationDelayMs = delayMs;
}


@Injectable({ scope: Scope.TRANSIENT })
export class Config6InstanceSpot implements ZLinkInstanceSpot {
  readonly context!: ZLinkInstanceSpotContext;

  configure(): void {
    objectEvidence?.add(`object-configure|rid=${objectEvidence.rid}`);
    try {
      this.context.handlers.addPacket(ObjectRequestHandler);
    } catch (error) {
      objectEvidence?.add(`object-configure-error|${error instanceof Error ? error.message : String(error)}`);
      throw error;
    }
  }

  async onInitialize(): Promise<void> {
    // The handler evidence records activation before dispatch so a failed
    // request distinguishes placement failure from packet registration.
    objectEvidence?.add(`object-initialized|rid=${objectEvidence.rid}`);
    await waitForScenarioGate('initialize', `rid=${objectEvidence?.rid ?? 'unknown'}`);
    if (objectActivationDelayMs > 0) {
      await new Promise((resolve) => setTimeout(resolve, objectActivationDelayMs));
    }
    await this.context.addTimer('config6-instance-timer', 100, Config6InstanceTimer);
  }
}

@Injectable()
export class Config6InstanceTimer implements ZLinkSpotTimerHandler<Config6InstanceSpot> {
  async handle(_spot: Config6InstanceSpot, _tick: ZLinkTimerTick): Promise<void> {
    objectEvidence?.add(`object-timer|rid=${objectEvidence.rid}`);
  }
}

@Injectable()
@ZLinkPacket('ObjectReq')
export class ObjectRequestHandler implements ZLinkSpotRequestHandler<ZLinkSpot, ObjectReq, ObjectRes> {
  async handle(spot: ZLinkSpot, request: ObjectReq, _context: import('@zlink-systems/framework').ZLinkMessageContext): Promise<ObjectRes> {
    const providerRid = objectEvidence?.rid ?? 'unknown';
    objectEvidence?.add(`object-request|rid=${providerRid}|spotId=${request.spotId}|operationId=${request.operationId}`);
    await waitForScenarioGate('request', `spotId=${request.spotId}|operationId=${request.operationId}`);
    if (request.operationId === '__close__') {
      await spot.context.close();
    }
    return {
      spotId: request.spotId,
      operationId: request.operationId,
      payload: request.payload,
      providerRid
    };
  }
}
