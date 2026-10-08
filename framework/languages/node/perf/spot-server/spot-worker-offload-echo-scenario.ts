import { Inject, Injectable, Scope } from '@nestjs/common';
import {
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_SPOT_MANAGER,
  ZLINK_SPOT_OUTBOUND,
  zlinkSpotPacketHandler
} from '@zlink-systems/nestjs';
import type {
  ZLinkMessageContext,
  ZLinkRouteMeshRuntime,
  ZLinkSpotManager,
  ZLinkSpotOutbound,
  ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import { PerfClock } from '../shared/clock';
import {
  DecimalText,
  PerfEchoReply,
  PerfEchoRequest,
  RoleConfig,
  WorkerObservation
} from '../shared/contracts';
import { LATENCY_SUFFIXES } from '../shared/histogram';
import { Measurement } from '../shared/measurement';
import { PayloadPattern } from '../shared/payload';
import { ScenarioMetrics } from '../server-support/scenario-metrics';
import { ObjectsReadiness, ROLE_CONFIG, runRole } from '../server-support/server-application';
import { runRequestStreams } from '../server-support/wait';
import { ActorlessSpot, configureSpotRole, createSpots, publishSpots } from './spot-role';

const WORKER_TIMINGS = Symbol('perf.WorkerTimings');
interface WorkerTimings {
  submitted: bigint;
  resumed: bigint;
  started: bigint;
  ended: bigint;
}

// §10.8 spot-worker-offload-echo. Question: how much a CPU worker call and the delivery of its result add to the
// local Spot echo of §10.7. Roles: HTTP Client x1, Spot process (Object Server + local driver + Framework worker,
// this file) x1; no remote echo process. The caller is the same local ZLinkSpotOutbound.requestToSpot as §10.7 and its
// interval is the primary latency; the Spot handler runs one runCpuWorker call per request:
// Yield (default) hands the turn back while the worker runs, ordinary keeps it. The worker callback is the
// self-contained xorshift32-v1 task of worker-task-millis (no sleep); it checks time and cancellation every 1024
// iterations. Public worker options carry min/max threads and idle timeout; Measurement owns the call deadline.
// worker-offload; payload 1024 bytes. Store: run Docker Redis (Spot addresses). Null: worker queue depth and Spot
// internals have no public observation; remote call, Actor, fanout do not apply.
export class SpotWorkerOffloadEchoScenario {
  private sequences: number[] = [];

  constructor(
    private readonly spots: ZLinkSpotOutbound,
    private readonly manager: ZLinkSpotManager,
    private readonly measurement: Measurement,
    private readonly config: RoleConfig,
    private readonly meshRuntime: ZLinkRouteMeshRuntime,
    private readonly readiness: ObjectsReadiness,
    private readonly metrics: ScenarioMetrics,
    private readonly workerTimings: Map<string, WorkerTimings>
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
          source: 'ZLinkSpotOutbound.requestToSpot -> runCpuWorker',
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
    let reply: PerfEchoReply;
    try {
      reply = await this.spots
        .requestToSpot(spotId, request)
        .timeout(measurement.callTimeout())
        .submit<PerfEchoReply>();
      PayloadPattern.validateIdentity(request, reply);
      measurement.pattern.validate(reply.payload);
    } catch (error) {
      measurement.completeOperation(started, error, PerfClock.now());
      this.workerTimings.delete(request.correlationId);
      return;
    }
    const completed = PerfClock.now();
    const windowSuccess = measurement.completeOperation(started, undefined, completed);
    const worker = this.workerTimings.get(request.correlationId);
    this.workerTimings.delete(request.correlationId);
    if (windowSuccess && measurement.phase === 'measured') {
      if (!worker)
        throw new Error(
          `The successful operation '${request.correlationId}' has no worker timing evidence.`
        );
      this.metrics.record('workerCallLatencyMs', worker.submitted, worker.resumed, completed);
      this.metrics.record('workerTaskLatencyMs', worker.started, worker.ended, completed);
    }
  }
}

@Injectable({ scope: Scope.TRANSIENT })
export class SpotWorkerOffloadSpot extends ActorlessSpot {}

// §10.8 xorshift32-v1: x=0x12345678; x^=x<<13; x^=x>>>17; x^=x<<5 in 32 bits. Time and cancellation are checked every
// 1024 iterations and the task ends at or after the target duration. It uses no sleep and no shared state.
// The Framework runs a CPU callback in a worker thread from its source text, so the callback captures nothing: the
// target duration is fixed into its source once at bootstrap, and it returns only cloneable values (no collector).
const WORKER_CLOCK_DOMAIN = 'worker-thread-hrtime';
export function xorshift32Work(taskMillis: number): (signal: AbortSignal) => WorkerObservation {
  const source = `function (signal) {
    const started = process.hrtime.bigint();
    const target = started + ${Math.trunc(taskMillis)}n * 1000000n;
    let x = 0x12345678;
    let iterations = 0;
    do {
      for (let i = 0; i < 1024; i++) {
        x ^= x << 13;
        x ^= x >>> 17;
        x ^= x << 5;
      }
      iterations += 1024;
      signal.throwIfAborted();
    } while (process.hrtime.bigint() < target);
    return { startedTicks: started.toString(), endedTicks: process.hrtime.bigint().toString(), clockDomainId: '${WORKER_CLOCK_DOMAIN}',
      iterations: String(iterations), checksum: x >>> 0 };
  }`;
  return new Function(`return (${source});`)() as (signal: AbortSignal) => WorkerObservation;
}

// The Spot handler: one CPU worker call, then the typed echo. The worker callback returns its own timing evidence.
@zlinkSpotPacketHandler({ spot: () => SpotWorkerOffloadSpot, packetName: 'PerfEchoRequest' })
@Injectable()
export class SpotWorkerOffloadHandler implements ZLinkSpotRequestHandler<
  SpotWorkerOffloadSpot,
  PerfEchoRequest,
  PerfEchoReply
> {
  private readonly work: (signal: AbortSignal) => WorkerObservation;

  constructor(
    @Inject(Measurement) private readonly measurement: Measurement,
    @Inject(ScenarioMetrics) private readonly metrics: ScenarioMetrics,
    @Inject(ROLE_CONFIG) private readonly config: RoleConfig,
    @Inject(WORKER_TIMINGS) private readonly workerTimings: Map<string, WorkerTimings>
  ) {
    this.work = xorshift32Work(config.worker!.taskMillis);
  }

  async handle(
    spot: SpotWorkerOffloadSpot,
    request: PerfEchoRequest,
    _context: ZLinkMessageContext
  ): Promise<PerfEchoReply> {
    const received = PerfClock.now();
    const { measurement, metrics, config } = this;
    measurement.handlerEnter();
    try {
      measurement.validateRequest(request);
      if (request.phase === 'measured') metrics.count('spot.applicationHandlerEntries');
      const submitted = PerfClock.now();
      const call = spot.context.runCpuWorker(this.work).timeoutMs(measurement.callTimeout());
      let observation: WorkerObservation;
      if (config.terminal === 'yield') {
        metrics.count('spot.applicationYieldCalls');
        observation = await call.yield();
      } else observation = await call.submit();
      const resumed = PerfClock.now();
      const timing = this.workerTiming(observation, submitted, resumed);
      const reply = PayloadPattern.reply(request, received);
      if (measurement.phase !== 'setup') this.workerTimings.set(request.correlationId, timing);
      measurement.recordReply(request);
      if (measurement.phase === 'setup' && !config.source)
        measurement.setupEvidence = [
          {
            kind: 'typedProbeReply',
            source: 'ZLinkSpotRequestHandler -> runCpuWorker',
            observedValue: {
              correlationId: request.correlationId,
              iterations: observation.iterations,
              checksum: observation.checksum
            }
          }
        ];
      return reply;
    } catch (error) {
      measurement.recordDiagnostic(error);
      throw error;
    } finally {
      measurement.handlerExit();
    }
  }

  private workerTiming(
    observation: WorkerObservation,
    submitted: bigint,
    resumed: bigint
  ): WorkerTimings {
    if (
      observation.clockDomainId !== WORKER_CLOCK_DOMAIN ||
      DecimalText.u64(observation.iterations) === 0n
    )
      throw new Error('The worker observation is not from the worker callback or is empty.');
    return {
      submitted,
      resumed,
      started: DecimalText.i64(observation.startedTicks),
      ended: DecimalText.i64(observation.endedTicks)
    };
  }
}

export async function runSpotWorkerOffloadEcho(config: RoleConfig): Promise<void> {
  if (config.role !== 'spot' || !config.source || !config.worker)
    throw new Error('The Spot role with worker config is the source of this scenario.');
  const worker = config.worker;
  const measurement = new Measurement(config, config.source);
  const workerTimings = new Map<string, WorkerTimings>();
  const clockKeys = ['worker.submitToStart', 'worker.resultToContinuation'].flatMap((prefix) =>
    LATENCY_SUFFIXES.map((suffix) => `${prefix}.${suffix}`)
  );
  const metrics = new ScenarioMetrics(measurement)
    .counters('spot.applicationHandlerEntries', 'spot.applicationYieldCalls')
    .latency('workerCallLatencyMs', 'worker.callLatency')
    .latency('workerTaskLatencyMs', 'worker.taskLatency')
    .markUnsupported(
      'CLOCK_DOMAIN_UNVERIFIED',
      'The worker thread clock is not verified to share the main thread epoch (§15.2); this interval crosses both.',
      ...clockKeys
    )
    .markHistogramsUnsupported(
      'CLOCK_DOMAIN_UNVERIFIED',
      'The worker thread clock is not verified to share the main thread epoch (§15.2); this interval crosses both.',
      'workerSubmitToStartMs',
      'workerResultToContinuationMs'
    )
    .markUnsupported(
      'PUBLIC_OBSERVATION_UNSUPPORTED',
      'Public worker options are settings; no queue depth snapshot exists.',
      'worker.pool.queueDepth.max',
      'worker.pool.queueDepth.mean'
    )
    .spotInternalsUnsupported()
    .provenance('workerOptions', {
      algorithm: worker.algorithm,
      taskMillis: worker.taskMillis,
      applied: {
        minThreads: worker.minThreads,
        maxThreads: worker.maxThreads,
        idleTimeoutMs: worker.idleTimeoutMs
      },
      callDeadlineRule: 'phaseEnd+drainTimeoutMs'
    });
  const readiness = new ObjectsReadiness(false, 'This cell has not created its User Spots yet.');
  let scenario: SpotWorkerOffloadEchoScenario | undefined;
  await runRole(
    {
      config,
      objects: readiness,
      // §5.2: only the public worker options; the Node.js options carry no queue length.
      worker: {
        minThreads: worker.minThreads,
        maxThreads: worker.maxThreads,
        idleTimeoutMs: worker.idleTimeoutMs
      },
      providers: [
        { provide: ScenarioMetrics, useValue: metrics },
        { provide: WORKER_TIMINGS, useValue: workerTimings },
        SpotWorkerOffloadSpot,
        SpotWorkerOffloadHandler
      ],
      configureFramework: (builder) =>
        configureSpotRole(builder, config, false, SpotWorkerOffloadSpot),
      workload: () => scenario?.run,
      prepare: async (app) => {
        scenario = new SpotWorkerOffloadEchoScenario(
          app.get<ZLinkSpotOutbound>(ZLINK_SPOT_OUTBOUND, { strict: false }),
          app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER, { strict: false }),
          measurement,
          config,
          app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }),
          readiness,
          metrics,
          workerTimings
        );
        await scenario.prepare();
      }
    },
    measurement
  );
}
