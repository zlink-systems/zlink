import { Injectable } from '@nestjs/common';
import {
  ZLinkPacket,
  ZLinkTimerOverrunPolicy,
  type ZLinkMessageContext,
  type ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import {
  SpotMsg,
  SpotServiceNames,
  spotServicePacket,
  type SpotAdminReq,
  type SpotAdminRes
} from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import type { ScenarioUserSpot } from '../Spots/scenario-spots';
import { BasicTimerHandler, IdleCloseTimerHandler, OverrunTimerHandler } from './timer-handlers';

@Injectable()
@ZLinkPacket('SpotAdminReq')
export class SpotAdminHandler implements ZLinkSpotRequestHandler<ScenarioUserSpot, SpotAdminReq, SpotAdminRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: ScenarioUserSpot, request: SpotAdminReq, _context: ZLinkMessageContext): Promise<SpotAdminRes> {
    const periodMs = request.periodMs ?? 10;
    switch (request.operation) {
      case 'publish':
        spot.context.outbound.publish(
          SpotServiceNames.spotChannel,
          SpotServiceNames.spotEventTopic,
          spotServicePacket(SpotMsg, { marker: request.marker ?? '' })
        ).submit();
        this.evidence.add(`spot-publish|rid=${this.evidence.rid}|spot=${spot.context.spotId}|marker=${request.marker}`);
        break;
      case 'worker': {
        const marker = await spot.context.runIoWorker(
          (signal) => delayWorker(request.marker ?? '', request.delayMs ?? 0, signal)
        ).yield();
        spot.add(100);
        this.evidence.add(`worker-complete|rid=${this.evidence.rid}|spot=${spot.context.spotId}|marker=${marker}`);
        break;
      }
      case 'idleTimer':
        await spot.context.addTimer(request.name ?? 'idle', periodMs, IdleCloseTimerHandler);
        break;
      case 'timer':
        await spot.context.addTimer(request.name ?? 'timer', periodMs, BasicTimerHandler);
        break;
      case 'overrunTimer':
        await spot.context.addTimer(request.name ?? 'overrun', periodMs, OverrunTimerHandler, {
          overrunPolicy: timerPolicy(request.policy), maxCatchUpTicks: 2
        });
        break;
    }
    return { spotId: String(spot.context.spotId), nodeRid: String(spot.context.nodeRid), marker: request.marker };
  }
}

function timerPolicy(policy: SpotAdminReq['policy']): ZLinkTimerOverrunPolicy {
  switch (policy) {
    case 'CatchUpBounded': return ZLinkTimerOverrunPolicy.CatchUpBounded;
    case 'DelayNextTick': return ZLinkTimerOverrunPolicy.DelayNextTick;
    default: return ZLinkTimerOverrunPolicy.SkipLateTicks;
  }
}

function delayWorker(marker: string, delayMs: number, signal: AbortSignal): Promise<string> {
  return new Promise((resolve, reject) => {
    if (signal.aborted) return reject(signal.reason);
    const timer = setTimeout(() => resolve(marker), delayMs);
    signal.addEventListener('abort', () => { clearTimeout(timer); reject(signal.reason); }, { once: true });
  });
}
