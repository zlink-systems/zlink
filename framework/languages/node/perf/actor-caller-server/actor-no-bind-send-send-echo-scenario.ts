import { Inject, Injectable } from '@nestjs/common';
import type {
  ZLinkActorClient,
  ZLinkActorManager,
  ZLinkMessageContext,
  ZLinkRouteMeshRuntime,
  ZLinkSendHandler
} from '@zlink-systems/framework';
import {
  ZLINK_ACTOR_CLIENT,
  ZLINK_ACTOR_MANAGER,
  ZLINK_ROUTE_MESH_RUNTIME
} from '@zlink-systems/nestjs';
import { PerfClock } from '../shared/clock';
import { DecimalText, PerfEchoReply, RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { ScenarioMetrics } from '../server-support/scenario-metrics';
import { SendSendCorrelation } from '../server-support/send-send-correlation';
import { ObjectsReadiness, runRole } from '../server-support/server-application';
import { runAdmissionStreams } from '../server-support/wait';
import { ActorCallerSetup } from './actor-caller-setup';

// §10.10 actor-no-bind-send-send-echo. Question: what do the source admission of a global-ActorId send and the
// application echo round trip cost, measured apart? Roles: HTTP Client x1 (trigger only), ActorCaller (Object Client
// plus the Server of a run/cell-only return ChannelName, this file) x1, Actor (Object Server) x1.
// One operation: the correlation is registered and sendToActor starts; it ends when the return Channel handler
// validates the echo (§13). Separately, actor.sourceAdmission.* is the same call's start to the sendToActor terminal.
// send-send, ordinary; 4096 bytes; one unbound Actor per logical stream. Store: run Docker Redis.
// Null: physical connections, Spot, worker, fanout; the remote mailbox acceptance time is not publicly observable.
export class ActorNoBindSendSendEchoScenario {
  private sequences: number[] = [];

  constructor(
    private readonly actorClient: ZLinkActorClient,
    private readonly measurement: Measurement,
    private readonly config: RoleConfig,
    private readonly setup: ActorCallerSetup,
    private readonly readiness: ObjectsReadiness,
    private readonly correlations: SendSendCorrelation,
    private readonly metrics: ScenarioMetrics
  ) {}

  async prepare(): Promise<void> {
    const { config, measurement } = this;
    const signal = AbortSignal.timeout(config.workload.setupTimeoutMs);
    try {
      const created = await this.setup.createActors(signal);
      this.sequences = new Array<number>(config.workload.logicalStreams as number).fill(0);
      let next = 0;
      // §5: probes per prepared target, bounded by --connect-concurrency.
      const worker = async (): Promise<void> => {
        for (;;) {
          const stream = next++;
          if (stream >= this.sequences.length) return;
          const request = measurement
            .request(stream, ++this.sequences[stream], true)
            .with({ returnChannel: config.channelName });
          const entry = this.correlations.register(request, PerfClock.now());
          await this.actorClient.sendToActor(config.actorIds[stream], request).submit(signal);
          const { error } = await this.correlations.completeAsync(entry);
          if (error) throw error;
        }
      };
      await Promise.all(
        Array.from(
          { length: Math.min(config.workload.connectConcurrency as number, this.sequences.length) },
          worker
        )
      );
      measurement.setupEvidence = [
        {
          kind: 'typedProbeEcho',
          source: 'ZLinkActorClient.sendToActor -> return Channel send handler',
          observedValue: { probes: this.sequences.length, streams: this.sequences.length }
        }
      ];
      // §16.1: objectsReady means the create and the probe echo of every Actor are done, so warmup starts on quiet roles.
      this.readiness.set(true, '', [created, ...measurement.setupEvidence]);
    } catch (error) {
      measurement.recordDiagnostic(error);
    }
  }

  run = (): Promise<void> =>
    runAdmissionStreams(
      this.config.workload.logicalStreams as number,
      () => this.measurement.canIssue,
      (stream) => this.loop(stream)
    );

  private async loop(stream: number): Promise<boolean> {
    const { config, measurement } = this;
    const actorId = config.actorIds[stream];
    if (!measurement.canIssue) return false;
    let request = measurement
      .request(stream, ++this.sequences[stream])
      .with({ returnChannel: config.channelName });
    const started = measurement.beginOperation('send');
    if (started === undefined) return false;
    request = request.with({ sentTicks: DecimalText.of(started) });
    const entry = this.correlations.register(request, started); // §13: registered right before the first public send
    try {
      await this.actorClient.sendToActor(actorId, request).submit();
      const admitted = PerfClock.now();
      this.metrics.record('sourceAdmissionMs', started, admitted);
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

// The return Channel handler of this caller: the Actor's echo arrives as a second one-way send.
@Injectable()
export class ActorReturnHandler implements ZLinkSendHandler<PerfEchoReply> {
  constructor(@Inject(SendSendCorrelation) private readonly correlations: SendSendCorrelation) {}

  async handle(message: PerfEchoReply, _context: ZLinkMessageContext): Promise<void> {
    this.correlations.reply(message);
  }
}

export async function runActorNoBindSendSendEcho(config: RoleConfig): Promise<void> {
  const measurement = new Measurement(config, config.source);
  const metrics = new ScenarioMetrics(measurement).latency(
    'sourceAdmissionMs',
    'actor.sourceAdmission.latency'
  );
  const correlations = new SendSendCorrelation(measurement, metrics);
  const readiness = new ObjectsReadiness(
    false,
    'Actors are not yet created and probed through the public API.'
  );
  let scenario: ActorNoBindSendSendEchoScenario | undefined;
  await runRole(
    {
      config,
      objects: readiness,
      providers: [{ provide: SendSendCorrelation, useValue: correlations }, ActorReturnHandler],
      configureFramework: (builder) => {
        const mesh = builder
          .addRouteMesh(config.meshName!)
          .setRoutingIdPrefix('perf-actor-caller')
          .listen(config.transportEndpoints.mesh)
          .setAdvertiseHost('127.0.0.1');
        mesh.objects().client();
        mesh
          .channel(config.channelName!)
          .server()
          .addSendHandler('PerfEchoReply', ActorReturnHandler);
      },
      workload: () => scenario?.run,
      prepare: async (app) => {
        const setup = new ActorCallerSetup(
          config,
          app.get<ZLinkActorManager>(ZLINK_ACTOR_MANAGER, { strict: false }),
          app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false })
        );
        scenario = new ActorNoBindSendSendEchoScenario(
          app.get<ZLinkActorClient>(ZLINK_ACTOR_CLIENT, { strict: false }),
          measurement,
          config,
          setup,
          readiness,
          correlations,
          metrics
        );
        await scenario.prepare();
      }
    },
    measurement
  );
}
