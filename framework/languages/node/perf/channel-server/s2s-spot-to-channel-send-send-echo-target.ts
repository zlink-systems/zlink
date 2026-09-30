import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_SPOT_OUTBOUND } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext, ZLinkSendHandler, ZLinkSpotOutbound } from '@zlink-systems/framework';
import { PerfClock } from '../shared/clock';
import { PerfEchoRequest, RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { PayloadPattern } from '../shared/payload';
import { runRole } from '../server-support/server-application';

// The Channel target of §10.6 (Object Client): the Channel send handler receives the Spot's send and answers with a
// second one-way send to the SpotId that the DTO names in returnSpotId. The measured operation lives in the Spot
// process; this side does not assume the Channel context carries the source SpotId.
export async function runS2sSpotToChannelSendSendEchoTarget(config: RoleConfig): Promise<void> {
  if (config.role !== 'channel' || config.source) throw new Error('The Channel role is the echo target of this scenario.');
  await runRole({
    config,
    providers: [S2sReturnToSpotHandler],
    configureFramework: (builder) => {
      const mesh = builder.addRouteMesh(config.meshName!).setRoutingIdPrefix('perf-channel').listen(config.transportEndpoints.mesh).setAdvertiseHost('127.0.0.1');
      mesh.objects().client();
      mesh.channel(config.channelName!).server().addSendHandler('PerfEchoRequest', S2sReturnToSpotHandler);
    }
  }, new Measurement(config, config.source));
}

@Injectable()
export class S2sReturnToSpotHandler implements ZLinkSendHandler<PerfEchoRequest> {
  constructor(@Inject(Measurement) private readonly measurement: Measurement, @Inject(ZLINK_SPOT_OUTBOUND) private readonly spots: ZLinkSpotOutbound) {}

  async handle(message: PerfEchoRequest, _context: ZLinkMessageContext): Promise<void> {
    const received = PerfClock.now();
    const measurement = this.measurement;
    measurement.handlerEnter();
    try {
      if (!message.returnSpotId) throw new Error('No return SpotId in the request.');
      measurement.validateRequest(message, null, message.returnSpotId);
      const reply = PayloadPattern.reply(message, received);
      measurement.recordApplicationCall(message, 'send');
      await this.spots.sendToSpot(message.returnSpotId, reply).submit();
      if (measurement.phase === 'setup') measurement.setupEvidence = [{ kind: 'typedProbeReply', source: 'ZLinkSendHandler<PerfEchoRequest> -> ZLinkSpotOutbound.sendToSpot', observedValue: message.correlationId }];
    } catch (error) {
      measurement.recordDiagnostic(error);
      throw error;
    } finally {
      measurement.handlerExit();
    }
  }
}
