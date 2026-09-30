import { Inject, Injectable, Scope } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext, ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import { PerfClock } from '../shared/clock';
import { PerfEchoReply, PerfEchoRequest, RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { PayloadPattern } from '../shared/payload';
import { ScenarioMetrics } from '../server-support/scenario-metrics';
import { ROLE_CONFIG } from '../server-support/server-application';
import { ActorlessSpot } from './spot-role';

// The User Spot that answers a typed PerfEchoRequest with the typed echo at once: the target of §10.3 and the
// local echo Spot of §10.7. The typed request handler is the only application code on the Spot.
@Injectable({ scope: Scope.TRANSIENT })
export class PerfEchoSpot extends ActorlessSpot {}

@zlinkSpotPacketHandler({ spot: () => PerfEchoSpot, packetName: 'PerfEchoRequest' })
@Injectable()
export class PerfEchoRequestHandler implements ZLinkSpotRequestHandler<PerfEchoSpot, PerfEchoRequest, PerfEchoReply> {
  constructor(
    @Inject(Measurement) private readonly measurement: Measurement, @Inject(ScenarioMetrics) private readonly metrics: ScenarioMetrics,
    @Inject(ROLE_CONFIG) private readonly config: RoleConfig
  ) {}

  async handle(_spot: PerfEchoSpot, request: PerfEchoRequest, _context: ZLinkMessageContext): Promise<PerfEchoReply> {
    const received = PerfClock.now();
    const measurement = this.measurement;
    measurement.handlerEnter();
    try {
      measurement.validateRequest(request);
      const reply = PayloadPattern.reply(request, received);
      measurement.recordReply(request);
      if (request.phase === 'measured') this.metrics.count('spot.applicationHandlerEntries');
      if (measurement.phase === 'setup' && !this.config.source) measurement.setupEvidence = [{ kind: 'typedProbeReply', source: 'ZLinkSpotRequestHandler<PerfEchoRequest,PerfEchoReply>', observedValue: request.correlationId }];
      return reply;
    } catch (error) {
      measurement.recordDiagnostic(error);
      throw error;
    } finally {
      measurement.handlerExit();
    }
  }
}
