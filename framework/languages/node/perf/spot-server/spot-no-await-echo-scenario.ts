import type {
  ZLinkRouteMeshRuntime,
  ZLinkSpotManager,
  ZLinkSpotOutbound
} from '@zlink-systems/framework';
import {
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_SPOT_MANAGER,
  ZLINK_SPOT_OUTBOUND
} from '@zlink-systems/nestjs';
import { DecimalText, PerfEchoReply, RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { PayloadPattern } from '../shared/payload';
import { ScenarioMetrics } from '../server-support/scenario-metrics';
import { ObjectsReadiness, runRole } from '../server-support/server-application';
import { runRequestStreams } from '../server-support/wait';
import { PerfEchoRequestHandler, PerfEchoSpot } from './perf-echo-spot';
import { configureSpotRole, createSpots, publishSpots } from './spot-role';

// §10.7 spot-no-await-echo (also the §11.3 local Spot reference). Question: what the public Spot outbound costs from
// the caller to a User Spot in the same process that echoes at once; not a pure mailbox cost. Roles: HTTP Client x1,
// Spot process (Object Server + local application driver, this file) x1; no Channel, Actor or worker. One operation:
// the local ZLinkSpotOutbound.requestToSpot until the typed echo is validated (codec and local dispatch included,
// the HTTP trigger is not). no-await: the caller is an ordinary request and the handler replies with the typed echo
// at once. Payload 1024 bytes. Setup: only this Object Server can place the Spots. Store: run Docker Redis (Spot
// addresses). Null: remote call, worker, Actor, fanout; mailbox depth and real turns have no public observation;
// the driver histogram is not kept because this interval is already the primary latency.
export class SpotNoAwaitEchoScenario {
  private sequences: number[] = [];

  constructor(
    private readonly spots: ZLinkSpotOutbound,
    private readonly manager: ZLinkSpotManager,
    private readonly measurement: Measurement,
    private readonly config: RoleConfig,
    private readonly meshRuntime: ZLinkRouteMeshRuntime,
    private readonly readiness: ObjectsReadiness
  ) {}

  async prepare(): Promise<void> {
    const { config, measurement } = this;
    const objects = await createSpots(config, this.manager, this.meshRuntime, measurement);
    if (!objects) return;
    try {
      this.sequences = new Array<number>(config.workload.logicalStreams as number).fill(0);
      const probes: unknown[] = [];
      for (let target = 0; target < config.spotIds.length; target++) {
        const request = measurement.request(
          target,
          ++this.sequences[target % this.sequences.length],
          true
        );
        const reply = await this.spots
          .requestToSpot(config.spotIds[target], request)
          .timeout(measurement.callTimeout())
          .submit<PerfEchoReply>();
        PayloadPattern.validateIdentity(request, reply);
        measurement.pattern.validate(reply.payload);
        probes.push({
          correlationId: request.correlationId,
          receivedTicks: reply.receivedTicks,
          clockDomainId: reply.clockDomainId
        });
      }
      publishSpots(this.readiness, objects); // objectsReady only after every probe, so warmup never overlaps one
      measurement.setupEvidence = [
        {
          kind: 'typedProbeEcho',
          source: 'ZLinkSpotOutbound.requestToSpot.submit<PerfEchoReply>',
          observedValue: probes
        }
      ];
    } catch (error) {
      measurement.recordDiagnostic(error);
    }
  }

  run = (): Promise<void> =>
    runRequestStreams(
      this.config.workload.logicalStreams as number,
      () => this.measurement.canIssue,
      (stream) => this.loop(stream),
      (error) => this.measurement.recordDiagnostic(error)
    );

  private async loop(stream: number): Promise<void> {
    const { config, measurement } = this;
    const spotId = config.spotIds[stream % config.spotIds.length];
    if (!measurement.canIssue) return;
    let request = measurement.request(stream, ++this.sequences[stream]);
    const started = measurement.beginOperation();
    if (started === undefined) return;
    request = request.with({ sentTicks: DecimalText.of(started) });
    try {
      const reply = await this.spots
        .requestToSpot(spotId, request)
        .timeout(measurement.callTimeout())
        .submit<PerfEchoReply>();
      PayloadPattern.validateIdentity(request, reply);
      measurement.pattern.validate(reply.payload);
      measurement.completeOperation(started);
    } catch (error) {
      measurement.completeOperation(started, error);
    }
  }
}

export async function runSpotNoAwaitEcho(config: RoleConfig): Promise<void> {
  if (config.role !== 'spot' || !config.source)
    throw new Error('The Spot role is the source of this scenario.');
  const measurement = new Measurement(config, config.source);
  const metrics = new ScenarioMetrics(measurement)
    .counters('spot.applicationHandlerEntries')
    .spotInternalsUnsupported();
  const readiness = new ObjectsReadiness(false, 'This cell has not created its User Spots yet.');
  let scenario: SpotNoAwaitEchoScenario | undefined;
  await runRole(
    {
      config,
      objects: readiness,
      providers: [
        { provide: ScenarioMetrics, useValue: metrics },
        PerfEchoSpot,
        PerfEchoRequestHandler
      ],
      configureFramework: (builder) => configureSpotRole(builder, config, false, PerfEchoSpot),
      workload: () => scenario?.run,
      prepare: async (app) => {
        scenario = new SpotNoAwaitEchoScenario(
          app.get<ZLinkSpotOutbound>(ZLINK_SPOT_OUTBOUND, { strict: false }),
          app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER, { strict: false }),
          measurement,
          config,
          app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }),
          readiness
        );
        await scenario.prepare();
      }
    },
    measurement
  );
}
