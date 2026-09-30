import type { ZLinkActorClient, ZLinkActorManager, ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { ZLINK_ACTOR_CLIENT, ZLINK_ACTOR_MANAGER, ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import { DecimalText, PerfEchoReply, RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { PayloadPattern } from '../shared/payload';
import { ObjectsReadiness, runRole } from '../server-support/server-application';
import { runLoops } from '../server-support/wait';
import { ActorCallerSetup } from './actor-caller-setup';

// §10.9 actor-no-bind-request-echo. Question: what do address lookup and a remote request cost when a global ActorId
// is reached without any Session binding? Roles: HTTP Client x1 (trigger only), ActorCaller (Object Client, this
// file) x1, Actor (Object Server) x1. One operation: requestToActor starts and ends when the typed echo has been
// validated. request, ordinary; 4096 bytes; one unbound Actor per logical stream, created during setup.
// Store: run Docker Redis. Null: physical connections, Spot, worker, fanout and actor.sourceAdmission (no send).
export class ActorNoBindRequestEchoScenario {
  private sequences: number[] = [];

  constructor(
    private readonly actorClient: ZLinkActorClient, private readonly measurement: Measurement, private readonly config: RoleConfig,
    private readonly setup: ActorCallerSetup, private readonly readiness: ObjectsReadiness
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
          const request = measurement.request(stream, ++this.sequences[stream], true);
          const reply = await this.actorClient.requestToActor(config.actorIds[stream], request).timeout(config.workload.setupTimeoutMs).submit<PerfEchoReply>(signal);
          PayloadPattern.validateIdentity(request, reply);
          measurement.pattern.validate(reply.payload);
        }
      };
      await Promise.all(Array.from({ length: Math.min(config.workload.connectConcurrency as number, this.sequences.length) }, worker));
      measurement.setupEvidence = [{ kind: 'typedProbeEcho', source: 'ZLinkActorClient.requestToActor.submit<PerfEchoReply>',
        observedValue: { probes: this.sequences.length, streams: this.sequences.length } }];
      // §16.1: objectsReady means the create and the probe echo of every Actor are done, so warmup starts on quiet roles.
      this.readiness.set(true, '', [created, ...measurement.setupEvidence]);
    } catch (error) {
      measurement.recordDiagnostic(error);
    }
  }

  run = (): Promise<void> => runLoops(this.config.workload.logicalStreams as number, this.config.workload.inflight, (stream) => this.loop(stream));

  private async loop(stream: number): Promise<void> {
    const { config, measurement } = this;
    const actorId = config.actorIds[stream];
    while (measurement.canIssue) {
      let request = measurement.request(stream, ++this.sequences[stream]);
      const started = measurement.beginOperation();
      if (started === undefined) break;
      request = request.with({ sentTicks: DecimalText.of(started) });
      try {
        const reply = await this.actorClient.requestToActor(actorId, request).timeout(config.workload.requestTimeoutMs).submit<PerfEchoReply>();
        PayloadPattern.validateIdentity(request, reply);
        measurement.pattern.validate(reply.payload);
        measurement.completeOperation(started);
      } catch (error) {
        measurement.completeOperation(started, error);
      }
    }
  }
}

export async function runActorNoBindRequestEcho(config: RoleConfig): Promise<void> {
  const measurement = new Measurement(config, config.source);
  const readiness = new ObjectsReadiness(false, 'Actors are not yet created and probed through the public API.');
  let scenario: ActorNoBindRequestEchoScenario | undefined;
  await runRole({
    config,
    objects: readiness,
    providers: [],
    configureFramework: (builder) => {
      builder.addRouteMesh(config.meshName!).setRoutingIdPrefix('perf-actor-caller').listen(config.transportEndpoints.mesh).setAdvertiseHost('127.0.0.1').objects().client();
    },
    workload: () => scenario?.run,
    prepare: async (app) => {
      const setup = new ActorCallerSetup(config, app.get<ZLinkActorManager>(ZLINK_ACTOR_MANAGER, { strict: false }), app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }));
      scenario = new ActorNoBindRequestEchoScenario(app.get<ZLinkActorClient>(ZLINK_ACTOR_CLIENT, { strict: false }), measurement, config, setup, readiness);
      await scenario.prepare();
    }
  }, measurement);
}
