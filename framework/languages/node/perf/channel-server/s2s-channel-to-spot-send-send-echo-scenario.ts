import { Inject, Injectable } from '@nestjs/common';
import type {
  ZLinkMessageContext,
  ZLinkRouteMeshRuntime,
  ZLinkSendHandler,
  ZLinkSpotManager,
  ZLinkSpotOutbound
} from '@zlink-systems/framework';
import {
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_SPOT_MANAGER,
  ZLINK_SPOT_OUTBOUND
} from '@zlink-systems/nestjs';
import { PerfClock } from '../shared/clock';
import { DecimalText, PerfEchoReply, RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { ScenarioMetrics } from '../server-support/scenario-metrics';
import { SendSendCorrelation } from '../server-support/send-send-correlation';
import { ObjectsReadiness, runRole } from '../server-support/server-application';
import { runTerminalStreams, sleep, until } from '../server-support/wait';

// §10.4 s2s-channel-to-spot-send-send-echo. Question: how completion rate, throughput and round trip differ from the
// request form of §10.3 when both directions are one-way sends. Roles: HTTP Client x1, Channel process (Object Client
// plus the Server of a run/cell-only return ChannelName, this file) x1, Spot process (Object Server) x1.
// One operation: the correlation is registered and the first sendToSpot starts; it ends when the return Channel
// handler validates the echo (§13). send-send, ordinary; payload 4096 bytes; streamId mod spotCount picks the Spot.
// Store: run Docker Redis. Null: physical connections, worker, Actor, fanout; Spot internals are not observable.
export class S2sChannelToSpotSendSendEchoScenario {
  private sequences: number[] = [];

  constructor(
    private readonly spots: ZLinkSpotOutbound,
    private readonly manager: ZLinkSpotManager,
    private readonly measurement: Measurement,
    private readonly config: RoleConfig,
    private readonly meshRuntime: ZLinkRouteMeshRuntime,
    private readonly readiness: ObjectsReadiness,
    private readonly correlations: SendSendCorrelation
  ) {}

  async prepare(): Promise<void> {
    const { config, measurement } = this;
    const timeoutMs = config.workload.setupTimeoutMs;
    try {
      await until(
        () => {
          const mesh = this.meshRuntime.snapshot(config.meshName!);
          return mesh.isReady && mesh.readyPeerCount > 0;
        },
        timeoutMs,
        'the RouteMesh to have a ready Object Server peer'
      );
      const found: unknown[] = [];
      const deadline = Date.now() + timeoutMs;
      for (const spotId of config.spotIds) {
        let spot;
        while ((spot = await this.manager.find(spotId)) === undefined) {
          if (Date.now() >= deadline)
            throw new Error(`Spot ${spotId} was not found inside setupTimeoutMs.`);
          await sleep(10);
        }
        found.push({ spotId, meshName: spot.meshName, nodeRid: spot.nodeRid ?? null });
      }
      this.sequences = new Array<number>(config.workload.logicalStreams as number).fill(0);
      const probes: unknown[] = [];
      for (let target = 0; target < config.spotIds.length; target++) {
        const request = measurement
          .request(target, ++this.sequences[target % this.sequences.length], true)
          .with({ returnChannel: config.channelName });
        const entry = this.correlations.register(request, PerfClock.now());
        await this.spots.sendToSpot(config.spotIds[target], request).submit();
        const { error } = await this.correlations.completeAsync(entry);
        if (error) throw error;
        probes.push({ correlationId: request.correlationId });
      }
      this.readiness.set(true, '', [
        { kind: 'spotFind', source: 'ZLinkSpotManager.find', observedValue: found }
      ]); // only after every probe
      measurement.setupEvidence = [
        {
          kind: 'typedProbeEcho',
          source: 'ZLinkSpotOutbound.sendToSpot -> return Channel send handler',
          observedValue: probes
        }
      ];
    } catch (error) {
      measurement.recordDiagnostic(error);
    }
  }

  run = (): Promise<void> =>
    runTerminalStreams(
      this.config.workload.logicalStreams as number,
      () => this.measurement.canIssue,
      (stream) => this.loop(stream)
    );

  private async loop(stream: number): Promise<boolean> {
    const { config, measurement } = this;
    const spotId = config.spotIds[stream % config.spotIds.length];
    if (!measurement.canIssue) return false;
    let request = measurement
      .request(stream, ++this.sequences[stream])
      .with({ returnChannel: config.channelName });
    const started = measurement.beginOperation('send');
    if (started === undefined) return false;
    request = request.with({ sentTicks: DecimalText.of(started) });
    const entry = this.correlations.register(request, started); // §13: registered right before the first public send
    try {
      await this.spots.sendToSpot(spotId, request).submit();
      this.correlations.firstSendEnded(entry, undefined);
    } catch (error) {
      this.correlations.firstSendEnded(entry, error);
    }
    void this.correlations.completeAsync(entry).then(({ error, completedTicks }) => {
      measurement.completeOperation(started, error, completedTicks);
    });
    return true;
  }
}

// The return Channel handler of this caller: the Spot's echo arrives as a second one-way send.
@Injectable()
export class S2sReturnHandler implements ZLinkSendHandler<PerfEchoReply> {
  constructor(@Inject(SendSendCorrelation) private readonly correlations: SendSendCorrelation) {}

  async handle(message: PerfEchoReply, _context: ZLinkMessageContext): Promise<void> {
    this.correlations.reply(message);
  }
}

export async function runS2sChannelToSpotSendSendEcho(config: RoleConfig): Promise<void> {
  if (config.role !== 'channel' || !config.source)
    throw new Error('The Channel role is the source of this scenario.');
  const measurement = new Measurement(config, config.source);
  const metrics = new ScenarioMetrics(measurement).spotInternalsUnsupported();
  const correlations = new SendSendCorrelation(measurement, metrics);
  const readiness = new ObjectsReadiness(
    false,
    'No User Spot has been found through the public manager yet.'
  );
  let scenario: S2sChannelToSpotSendSendEchoScenario | undefined;
  await runRole(
    {
      config,
      objects: readiness,
      providers: [{ provide: SendSendCorrelation, useValue: correlations }, S2sReturnHandler],
      configureFramework: (builder) => {
        const mesh = builder
          .addRouteMesh(config.meshName!)
          .setRoutingIdPrefix('perf-channel')
          .listen(config.transportEndpoints.mesh)
          .setAdvertiseHost('127.0.0.1');
        mesh.objects().client();
        mesh
          .channel(config.channelName!)
          .server()
          .addSendHandler('PerfEchoReply', S2sReturnHandler);
      },
      workload: () => scenario?.run,
      prepare: async (app) => {
        scenario = new S2sChannelToSpotSendSendEchoScenario(
          app.get<ZLinkSpotOutbound>(ZLINK_SPOT_OUTBOUND, { strict: false }),
          app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER, { strict: false }),
          measurement,
          config,
          app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }),
          readiness,
          correlations
        );
        await scenario.prepare();
      }
    },
    measurement
  );
}
