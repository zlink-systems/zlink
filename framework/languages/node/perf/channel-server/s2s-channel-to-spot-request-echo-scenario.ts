import type { ZLinkRouteMeshRuntime, ZLinkSpotManager, ZLinkSpotOutbound } from '@zlink-systems/framework';
import { ZLINK_ROUTE_MESH_RUNTIME, ZLINK_SPOT_MANAGER, ZLINK_SPOT_OUTBOUND } from '@zlink-systems/nestjs';
import { DecimalText, PerfEchoReply, RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { PayloadPattern } from '../shared/payload';
import { ScenarioMetrics } from '../server-support/scenario-metrics';
import { ObjectsReadiness, runRole } from '../server-support/server-application';
import { runLoops, sleep, until } from '../server-support/wait';

// §10.3 s2s-channel-to-spot-request-echo. Question: what a remote request to a global SpotId costs end to end
// (address lookup, delivery, reply). Roles: HTTP trigger Client x1, Channel process (Object Client, this file) x1,
// Spot process (Object Server, SpotServer) x1. One operation starts at the Channel's requestToSpot and ends when the
// typed echo is validated. request, ordinary; representative payload 4096 bytes. streamId mod spotCount picks the
// User Spot. Store: run Docker Redis (automatic discovery, Spot addresses). Null: physical connections, worker,
// Actor, fanout; Spot mailbox and turn internals have no public observation.
export class S2sChannelToSpotRequestEchoScenario {
  private sequences: number[] = [];

  constructor(
    private readonly spots: ZLinkSpotOutbound, private readonly manager: ZLinkSpotManager, private readonly measurement: Measurement,
    private readonly config: RoleConfig, private readonly meshRuntime: ZLinkRouteMeshRuntime, private readonly readiness: ObjectsReadiness
  ) {}

  async prepare(): Promise<void> {
    const { config, measurement } = this;
    const timeoutMs = config.workload.setupTimeoutMs;
    try {
      // Public status and the manager's resolve are polled; the probe call itself is never retried.
      await until(() => { const mesh = this.meshRuntime.snapshot(config.meshName!); return mesh.isReady && mesh.readyPeerCount > 0; }, timeoutMs, 'the RouteMesh to have a ready Object Server peer');
      const found: unknown[] = [];
      const deadline = Date.now() + timeoutMs;
      for (const spotId of config.spotIds) {
        let spot;
        while ((spot = await this.manager.find(spotId)) === undefined) {
          if (Date.now() >= deadline) throw new Error(`Spot ${spotId} was not found inside setupTimeoutMs.`);
          await sleep(10);
        }
        found.push({ spotId, meshName: spot.meshName, nodeRid: spot.nodeRid ?? null });
      }
      this.sequences = new Array<number>(config.workload.logicalStreams as number).fill(0);
      const probes: unknown[] = [];
      for (let target = 0; target < config.spotIds.length; target++) {
        const request = measurement.request(target, ++this.sequences[target % this.sequences.length], true);
        const reply = await this.spots.requestToSpot(config.spotIds[target], request).timeout(config.workload.requestTimeoutMs).submit<PerfEchoReply>();
        PayloadPattern.validateIdentity(request, reply);
        measurement.pattern.validate(reply.payload);
        probes.push({ correlationId: request.correlationId, receivedTicks: reply.receivedTicks, clockDomainId: reply.clockDomainId });
      }
      this.readiness.set(true, '', [{ kind: 'spotFind', source: 'ZLinkSpotManager.find', observedValue: found }]); // only after every probe
      measurement.setupEvidence = [{ kind: 'typedProbeEcho', source: 'ZLinkSpotOutbound.requestToSpot.submit<PerfEchoReply>', observedValue: probes }];
    } catch (error) {
      measurement.recordDiagnostic(error);
    }
  }

  run = (): Promise<void> => runLoops(this.config.workload.logicalStreams as number, this.config.workload.inflight, (stream) => this.loop(stream));

  private async loop(stream: number): Promise<void> {
    const { config, measurement } = this;
    const spotId = config.spotIds[stream % config.spotIds.length];
    while (measurement.canIssue) {
      let request = measurement.request(stream, ++this.sequences[stream]);
      const started = measurement.beginOperation();
      if (started === undefined) break;
      request = request.with({ sentTicks: DecimalText.of(started) });
      try {
        const reply = await this.spots.requestToSpot(spotId, request).timeout(config.workload.requestTimeoutMs).submit<PerfEchoReply>();
        PayloadPattern.validateIdentity(request, reply);
        measurement.pattern.validate(reply.payload);
        measurement.completeOperation(started);
      } catch (error) {
        measurement.completeOperation(started, error);
      }
    }
  }
}

export async function runS2sChannelToSpotRequestEcho(config: RoleConfig): Promise<void> {
  if (config.role !== 'channel' || !config.source) throw new Error('The Channel role is the source of this scenario.');
  const measurement = new Measurement(config, config.source);
  new ScenarioMetrics(measurement).spotInternalsUnsupported();
  const readiness = new ObjectsReadiness(false, 'No User Spot has been found through the public manager yet.');
  let scenario: S2sChannelToSpotRequestEchoScenario | undefined;
  await runRole({
    config,
    objects: readiness,
    providers: [],
    configureFramework: (builder) => {
      builder.addRouteMesh(config.meshName!).setRoutingIdPrefix('perf-channel').listen(config.transportEndpoints.mesh).setAdvertiseHost('127.0.0.1').objects().client();
    },
    workload: () => scenario?.run,
    prepare: async (app) => {
      scenario = new S2sChannelToSpotRequestEchoScenario(app.get<ZLinkSpotOutbound>(ZLINK_SPOT_OUTBOUND, { strict: false }),
        app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER, { strict: false }), measurement, config,
        app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }), readiness);
      await scenario.prepare();
    }
  }, measurement);
}
