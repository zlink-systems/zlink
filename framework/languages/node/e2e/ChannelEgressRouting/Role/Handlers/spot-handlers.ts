import { Inject, Injectable } from '@nestjs/common';
import {
  ZLinkPacket,
  type ZLinkChannelClient,
  type ZLinkMessageContext,
  type ZLinkSpotRequestHandler,
  type ZLinkSpotTimerHandler,
  type ZLinkTimer,
  type ZLinkTimerTick
} from '@zlink-systems/framework';
import { ZLINK_CHANNEL_CLIENT } from '@zlink-systems/nestjs';
import { ChannelEgressNames, SpotWorkflowReq, type SpotWorkflowRes } from '../../Shared/messages';
import type { Config12Spot } from '../Spots/config12-spot';
import { EvidenceStore } from '../Support/evidence-store';

const timers = new WeakMap<Config12Spot, ZLinkTimer>();

@Injectable()
@ZLinkPacket('SpotWorkflowReq')
export class SpotWorkflowHandler implements ZLinkSpotRequestHandler<Config12Spot, SpotWorkflowReq, SpotWorkflowRes> {
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_CHANNEL_CLIENT) private readonly channels: ZLinkChannelClient
  ) {}

  async handle(spot: Config12Spot, request: SpotWorkflowReq, _context: ZLinkMessageContext): Promise<SpotWorkflowRes> {
    const sequence = ['handler-start'];
    this.evidence.add(`spot-handler-start|spot=${spot.context.spotId}|id=${request.id}`);
    await this.channels
      .requestToChannel(ChannelEgressNames.workflow, new SpotWorkflowReq(`${request.id}-workflow`))
      .timeout(5000)
      .submit<SpotWorkflowRes>();
    sequence.push('workflow-reply', 'handler-end');
    this.evidence.add(`spot-workflow-reply|spot=${spot.context.spotId}|id=${request.id}`);
    this.evidence.add(`spot-handler-end|spot=${spot.context.spotId}|id=${request.id}`);
    const timer = await spot.context.addTimer(request.timerName, 1, SpotWorkflowTimerHandler);
    timers.set(spot, timer);
    this.evidence.add(`spot-timer-start|spot=${spot.context.spotId}|id=${request.id}|sequence=${sequence.join(',')}`);
    return { id: request.id, sequence };
  }
}

@Injectable()
export class SpotWorkflowTimerHandler implements ZLinkSpotTimerHandler<Config12Spot> {
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_CHANNEL_CLIENT) private readonly channels: ZLinkChannelClient
  ) {}

  async handle(spot: Config12Spot, tick: ZLinkTimerTick): Promise<void> {
    const id = String(spot.context.spotId);
    await this.channels
      .requestToChannel(ChannelEgressNames.workflow, new SpotWorkflowReq(`${id}-timer-workflow`))
      .timeout(5000)
      .submit<SpotWorkflowRes>();
    this.evidence.add(`spot-timer-workflow-reply|spot=${id}|timer=${tick.name}`);
    this.evidence.add(`spot-timer-end|spot=${id}|timer=${tick.name}|sequence=handler-start,workflow-reply,handler-end,timer-start,workflow-reply,timer-end`);
    const timer = timers.get(spot);
    if (timer !== undefined) {
      await timer.cancel();
      timers.delete(spot);
    }
  }
}
