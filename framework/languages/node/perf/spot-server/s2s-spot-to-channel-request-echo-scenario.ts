import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ROUTE_MESH_RUNTIME, ZLINK_SPOT_MANAGER, ZLINK_SPOT_OUTBOUND, zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext, ZLinkRouteMeshRuntime, ZLinkSpotManager, ZLinkSpotOutbound, ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import { PerfClock } from '../shared/clock';
import { PerfDriveReply, PerfDriveRequest, PerfEchoReply, PerfEchoRequest, RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { PayloadPattern } from '../shared/payload';
import { ScenarioMetrics } from '../server-support/scenario-metrics';
import { ObjectsReadiness, ROLE_CONFIG, runRole } from '../server-support/server-application';
import { runLoops, until } from '../server-support/wait';
import { ActorlessSpot, configureSpotRole, createSpots, publishSpots } from './spot-role';

// §10.5 s2s-spot-to-channel-request-echo. Question: how the terminal (ordinary or Yield) and the number of Spots
// change completion throughput, tail latency and the progress of other callbacks for the same Spot -> Channel remote
// request. Roles: HTTP Client x1, Spot process (Object Server + local public driver, this file) x1, Channel echo
// target x1. The driver sends PerfDriveRequest to the Spot with ZLinkSpotOutbound.requestToSpot; the Spot handler
// (an Actor-less SpotWide User Spot) makes one requestToChannel. The measured operation starts right before that
// remote call and ends after the reply is validated (after the turn is regained); driver time is kept apart as
// driver.latency.*. Streams are assigned to SpotIds round-robin, so no Actor queue takes part.
// request; ordinary or yield x 1 or 16 Spots; payload 4096 bytes. Store: run Docker Redis (Spot addresses and
// automatic mesh). Null: physical connections, worker, Actor, fanout; suspended/resumed turns, resume latency and
// mailbox depth have no public observation. Yield calls are the application's calls, not proven turn suspensions.
export class S2sSpotToChannelRequestEchoScenario {
  private sequences: number[] = [];

  constructor(
    private readonly spots: ZLinkSpotOutbound, private readonly manager: ZLinkSpotManager, private readonly measurement: Measurement,
    private readonly config: RoleConfig, private readonly meshRuntime: ZLinkRouteMeshRuntime, private readonly readiness: ObjectsReadiness,
    private readonly metrics: ScenarioMetrics
  ) {}

  async prepare(): Promise<void> {
    const { config, measurement } = this;
    const objects = await createSpots(config, this.manager, this.meshRuntime, measurement);
    if (!objects) return;
    try {
      await until(() => this.meshRuntime.snapshot(config.meshName!).channels.some((channel) => channel.channelName === config.channelName && channel.isReady && channel.readyTargetCount > 0),
        config.workload.setupTimeoutMs, 'the echo Channel target to be ready');
      this.sequences = new Array<number>(config.workload.logicalStreams as number).fill(0);
      const probes: unknown[] = [];
      for (let target = 0; target < config.spotIds.length; target++) {
        const echo = measurement.request(target, ++this.sequences[target % this.sequences.length], true);
        const driven = await this.spots.requestToSpot(config.spotIds[target], new PerfDriveRequest(echo)).timeout(config.workload.driverTimeoutMs).submit<PerfDriveReply>();
        if (!driven.started || driven.echo === null) throw new Error('The setup probe did not reach the Channel.');
        PayloadPattern.validateIdentity(echo, driven.echo);
        measurement.pattern.validate(driven.echo.payload);
        probes.push({ correlationId: echo.correlationId, receivedTicks: driven.echo.receivedTicks, clockDomainId: driven.echo.clockDomainId });
      }
      publishSpots(this.readiness, objects); // objectsReady only after every probe, so warmup never overlaps one
      measurement.setupEvidence = [{ kind: 'typedProbeEcho', source: 'ZLinkSpotOutbound.requestToSpot -> Spot requestToChannel', observedValue: probes }];
    } catch (error) {
      measurement.recordDiagnostic(error);
    }
  }

  run = (): Promise<void> => runLoops(this.config.workload.logicalStreams as number, this.config.workload.inflight, (stream) => this.loop(stream));

  // The local driver: one PerfDriveRequest per operation; the in-flight slot is the driver's until the handler returns.
  private async loop(stream: number): Promise<void> {
    const { config, measurement, metrics } = this;
    const spotId = config.spotIds[stream % config.spotIds.length];
    while (measurement.canIssue) {
      const echo = measurement.request(stream, ++this.sequences[stream]);
      const started = PerfClock.now();
      metrics.count('driver.issued');
      let driven: PerfDriveReply;
      try {
        driven = await this.spots.requestToSpot(spotId, new PerfDriveRequest(echo)).timeout(config.workload.driverTimeoutMs).submit<PerfDriveReply>();
      } catch (error) {
        metrics.count('driver.failed');
        measurement.recordDiagnostic(error);
        continue;
      }
      if (!driven.started) { metrics.count('driver.notStarted'); continue; }
      if (driven.echo !== null) metrics.record('driverLatencyMs', started, PerfClock.now());
    }
  }
}

@Injectable({ scope: Scope.TRANSIENT })
export class S2sRemoteRequestSpot extends ActorlessSpot {}

// The Spot handler: one requestToChannel per drive request. Ordinary keeps the Spot turn until the reply; Yield hands
// the turn back while the reply is pending (execution gate contract). This is the measured operation.
@zlinkSpotPacketHandler({ spot: () => S2sRemoteRequestSpot, packetName: 'PerfDriveRequest' })
@Injectable()
export class S2sRemoteRequestDriveHandler implements ZLinkSpotRequestHandler<S2sRemoteRequestSpot, PerfDriveRequest, PerfDriveReply> {
  constructor(
    @Inject(Measurement) private readonly measurement: Measurement, @Inject(ScenarioMetrics) private readonly metrics: ScenarioMetrics,
    @Inject(ROLE_CONFIG) private readonly config: RoleConfig
  ) {}

  async handle(spot: S2sRemoteRequestSpot, drive: PerfDriveRequest, _context: ZLinkMessageContext): Promise<PerfDriveReply> {
    const { measurement, metrics, config } = this;
    measurement.handlerEnter();
    try {
      let request = drive.echo;
      measurement.validateRequest(request);
      if (request.phase === 'measured') metrics.count('spot.applicationHandlerEntries');
      const probe = measurement.phase === 'setup'; // the setup probe is no measured operation
      let started = PerfClock.now();
      if (!probe) {
        const begun = measurement.beginOperation();
        if (begun === undefined) return new PerfDriveReply(false, null);
        started = begun;
      }
      request = new PerfEchoRequest({ ...request, sentTicks: started.toString() }); // decoded requests are plain objects
      try {
        const call = spot.context.outbound.requestToChannel(config.channelName!, request).timeout(config.workload.requestTimeoutMs);
        let reply: PerfEchoReply;
        if (config.terminal === 'yield') {
          metrics.count('spot.applicationYieldCalls');
          reply = await call.yield<PerfEchoReply>();
        } else reply = await call.submit<PerfEchoReply>();
        PayloadPattern.validateIdentity(request, reply);
        measurement.pattern.validate(reply.payload);
        if (!probe) measurement.completeOperation(started);
        return new PerfDriveReply(true, reply);
      } catch (error) {
        if (probe) throw error;
        measurement.completeOperation(started, error);
        return new PerfDriveReply(true, null);
      }
    } finally {
      measurement.handlerExit();
    }
  }
}

export async function runS2sSpotToChannelRequestEcho(config: RoleConfig): Promise<void> {
  if (config.role !== 'spot' || !config.source) throw new Error('The Spot role is the source of this scenario.');
  const measurement = new Measurement(config, config.source);
  const metrics = new ScenarioMetrics(measurement)
    .counters('driver.issued', 'driver.notStarted', 'driver.failed', 'spot.applicationHandlerEntries', 'spot.applicationYieldCalls')
    .latency('driverLatencyMs', 'driver.latency').aliasLatency('latency', 'spot.remoteCallLatency').spotInternalsUnsupported();
  const readiness = new ObjectsReadiness(false, 'This cell has not created its User Spots yet.');
  let scenario: S2sSpotToChannelRequestEchoScenario | undefined;
  await runRole({
    config,
    objects: readiness,
    providers: [{ provide: ScenarioMetrics, useValue: metrics }, S2sRemoteRequestSpot, S2sRemoteRequestDriveHandler],
    configureFramework: (builder) => configureSpotRole(builder, config, true, S2sRemoteRequestSpot),
    workload: () => scenario?.run,
    prepare: async (app) => {
      scenario = new S2sSpotToChannelRequestEchoScenario(app.get<ZLinkSpotOutbound>(ZLINK_SPOT_OUTBOUND, { strict: false }),
        app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER, { strict: false }), measurement, config,
        app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }), readiness, metrics);
      await scenario.prepare();
    }
  }, measurement);
}
