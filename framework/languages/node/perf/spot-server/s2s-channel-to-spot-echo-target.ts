import { Inject, Injectable, Scope } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import { ZLINK_ROUTE_MESH_RUNTIME, ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import type {
  ZLinkMessageContext,
  ZLinkRouteMeshRuntime,
  ZLinkSpotManager,
  ZLinkSpotPacketHandler
} from '@zlink-systems/framework';
import { PerfClock } from '../shared/clock';
import { PerfEchoRequest, RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { PayloadPattern } from '../shared/payload';
import { ScenarioMetrics } from '../server-support/scenario-metrics';
import { ObjectsReadiness, ROLE_CONFIG, runRole } from '../server-support/server-application';
import { PerfEchoRequestHandler, PerfEchoSpot } from './perf-echo-spot';
import { ActorlessSpot, configureSpotRole, createSpots, publishSpots } from './spot-role';

// The Spot process of §10.3 and §10.4 (Object Server, no driver): User Spots that answer a request with a typed
// echo (§10.3) or answer a send by sending the echo to the caller's return Channel (§10.4). The measured
// operation lives in the Channel process; this side only echoes.
export async function runS2sChannelToSpotEchoTarget(config: RoleConfig): Promise<void> {
  const request = config.scenario === 's2s-channel-to-spot-request-echo';
  const measurement = new Measurement(config, config.source);
  const metrics = new ScenarioMetrics(measurement).spotInternalsUnsupported();
  const readiness = new ObjectsReadiness(false, 'This cell has not created its User Spots yet.');
  await runRole(
    {
      config,
      objects: readiness,
      providers: [
        { provide: ScenarioMetrics, useValue: metrics },
        { provide: ObjectsReadiness, useValue: readiness },
        ...(request
          ? [PerfEchoSpot, PerfEchoRequestHandler]
          : [S2sSendEchoSpot, S2sSendEchoHandler])
      ],
      // callsChannel: the echo of §10.4 goes to the caller's return ChannelName.
      configureFramework: (builder) =>
        configureSpotRole(builder, config, !request, request ? PerfEchoSpot : S2sSendEchoSpot),
      prepare: async (app) => {
        const objects = await createSpots(
          config,
          app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER, { strict: false }),
          app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }),
          measurement
        );
        if (objects) publishSpots(readiness, objects);
      }
    },
    measurement
  );
}

@Injectable({ scope: Scope.TRANSIENT })
export class S2sSendEchoSpot extends ActorlessSpot {}

// §10.4: the echo goes back as a second one-way send to the caller's own return ChannelName (in the DTO).
@zlinkSpotPacketHandler({ spot: () => S2sSendEchoSpot, packetName: 'PerfEchoRequest' })
@Injectable()
export class S2sSendEchoHandler implements ZLinkSpotPacketHandler<
  S2sSendEchoSpot,
  PerfEchoRequest
> {
  constructor(
    @Inject(Measurement) private readonly measurement: Measurement,
    @Inject(ROLE_CONFIG) private readonly config: RoleConfig
  ) {}

  async handle(
    spot: S2sSendEchoSpot,
    message: PerfEchoRequest,
    _context: ZLinkMessageContext
  ): Promise<void> {
    const received = PerfClock.now();
    const measurement = this.measurement;
    measurement.handlerEnter();
    try {
      measurement.validateRequest(message, this.config.channelName);
      if (!message.returnChannel) throw new Error('No return Channel in the request.');
      const reply = PayloadPattern.reply(message, received);
      measurement.recordApplicationCall(message, 'send');
      await spot.context.outbound.sendToChannel(message.returnChannel, reply).submit();
      if (measurement.phase === 'setup' && !this.config.source)
        measurement.setupEvidence = [
          {
            kind: 'typedProbeReply',
            source: 'ZLinkSpotPacketHandler<PerfEchoRequest> -> sendToChannel',
            observedValue: message.correlationId
          }
        ];
    } catch (error) {
      measurement.recordDiagnostic(error);
      throw error;
    } finally {
      measurement.handlerExit();
    }
  }
}
