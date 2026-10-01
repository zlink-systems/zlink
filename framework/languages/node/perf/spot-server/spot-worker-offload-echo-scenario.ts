import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ROUTE_MESH_RUNTIME, ZLINK_SPOT_MANAGER, ZLINK_SPOT_OUTBOUND, zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext, ZLinkRouteMeshRuntime, ZLinkSpotManager, ZLinkSpotOutbound, ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import { PerfClock } from '../shared/clock';
import { DecimalText, PerfEchoReply, PerfEchoRequest, RoleConfig, WorkerObservation } from '../shared/contracts';
import { LATENCY_SUFFIXES } from '../shared/histogram';
import { Measurement } from '../shared/measurement';
import { PayloadPattern } from '../shared/payload';
import { ScenarioMetrics } from '../server-support/scenario-metrics';
import { ObjectsReadiness, ROLE_CONFIG, runRole } from '../server-support/server-application';
import { runLoops } from '../server-support/wait';
import { ActorlessSpot, configureSpotRole, createSpots, publishSpots } from './spot-role';

// §10.8 spot-worker-offload-echo. Question: how much a CPU worker call and the delivery of its result add to the
// local Spot echo of §10.7. Roles: HTTP Client x1, Spot process (Object Server + local driver + Framework worker,
// this file) x1; no remote echo process. The caller is the same local ZLinkSpotOutbound.requestToSpot as §10.7 and its
// interval is the primary latency; the Spot handler runs one runCpuWorker call per request:
// Yield (default) hands the turn back while the worker runs, ordinary keeps it. The worker callback is the
// self-contained xorshift32-v1 task of worker-task-millis (no sleep); it checks time and cancellation every 1024
// iterations. Public worker options carry min/max threads and idle timeout; the call timeout is the workerTimeoutMs.
// worker-offload; payload 1024 bytes. Store: run Docker Redis (Spot addresses). Null: worker queue depth and Spot
// internals have no public observation; remote call, Actor, fanout do not apply.
export class SpotWorkerOffloadEchoScenario {
  private sequences: number[] = [];

  constructor(
    private readonly spots: ZLinkSpotOutbound, private readonly manager: ZLinkSpotManager, private readonly measurement: Measurement,
    private readonly config: RoleConfig, private readonly meshRuntime: ZLinkRouteMeshRuntime, private readonly readiness: ObjectsReadiness
  ) {}

  async prepare(): Promise<void> {
    const { config, measurement } = this;
    const objects = await createSpots(config, this.manager, this.meshRuntime, measurement);
    if (!objects) return;
    try {
      this.sequences = new Array<number>(config.workload.logicalStreams as number).fill(0);
      const probes: unknown[] = [];
      for (let target = 0; target < config.spotIds.length; target++) {
        const request = measurement.request(target, ++this.sequences[target % this.sequences.length], true);
        const reply = await this.spots.requestToSpot(config.spotIds[target], request).timeout(config.workload.requestTimeoutMs).submit<PerfEchoReply>();
        PayloadPattern.validateIdentity(request, reply);
        measurement.pattern.validate(reply.payload);
        probes.push({ correlationId: request.correlationId, receivedTicks: reply.receivedTicks, clockDomainId: reply.clockDomainId });
      }
      publishSpots(this.readiness, objects); // objectsReady only after every probe, so warmup never overlaps one
      measurement.setupEvidence = [{ kind: 'typedProbeEcho', source: 'ZLinkSpotOutbound.requestToSpot -> runCpuWorker', observedValue: probes }];
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
export class SpotWorkerOffloadHandler implements ZLinkSpotRequestHandler<SpotWorkerOffloadSpot, PerfEchoRequest, PerfEchoReply> {
  private readonly work: (signal: AbortSignal) => WorkerObservation;

  constructor(
    @Inject(Measurement) private readonly measurement: Measurement, @Inject(ScenarioMetrics) private readonly metrics: ScenarioMetrics,
    @Inject(ROLE_CONFIG) private readonly config: RoleConfig
  ) {
    this.work = xorshift32Work(config.worker!.taskMillis);
  }

  async handle(spot: SpotWorkerOffloadSpot, request: PerfEchoRequest, _context: ZLinkMessageContext): Promise<PerfEchoReply> {
    const received = PerfClock.now();
    const { measurement, metrics, config } = this;
    measurement.handlerEnter();
    try {
      measurement.validateRequest(request);
      if (request.phase === 'measured') metrics.count('spot.applicationHandlerEntries');
      const submitted = PerfClock.now();
      const call = spot.context.runCpuWorker(this.work).timeoutMs(config.worker!.workerTimeoutMs);
      let observation: WorkerObservation;
      if (config.terminal === 'yield') {
        metrics.count('spot.applicationYieldCalls');
        observation = await call.yield();
      } else observation = await call.submit();
      const resumed = PerfClock.now();
      this.recordWorker(request, observation, submitted, resumed);
      const reply = PayloadPattern.reply(request, received);
      measurement.recordReply(request);
      if (measurement.phase === 'setup' && !config.source) measurement.setupEvidence = [{ kind: 'typedProbeReply', source: 'ZLinkSpotRequestHandler -> runCpuWorker',
        observedValue: { correlationId: request.correlationId, iterations: observation.iterations, checksum: observation.checksum } }];
      return reply;
    } catch (error) {
      measurement.recordDiagnostic(error);
      throw error;
    } finally {
      measurement.handlerExit();
    }
  }

  // The intervals of one worker call in the window. The worker thread's clock is not verified to share the main
  // thread's epoch, so only the two intervals that stay inside one clock are recorded (§10.8): the caller-side call and
  // the callback's own start-to-end. Submit-to-start and end-to-resume are never subtracted across the two clocks.
  private recordWorker(request: PerfEchoRequest, observation: WorkerObservation, submitted: bigint, resumed: bigint): void {
    if (request.phase !== 'measured') return;
    if (observation.clockDomainId !== WORKER_CLOCK_DOMAIN || DecimalText.u64(observation.iterations) === 0n)
      throw new Error('The worker observation is not from the worker callback or is empty.');
    const started = DecimalText.i64(observation.startedTicks);
    const ended = DecimalText.i64(observation.endedTicks);
    this.metrics.record('workerCallLatencyMs', submitted, resumed);
    this.metrics.record('workerTaskLatencyMs', started, ended, resumed);
  }
}

export async function runSpotWorkerOffloadEcho(config: RoleConfig): Promise<void> {
  if (config.role !== 'spot' || !config.source || !config.worker) throw new Error('The Spot role with worker config is the source of this scenario.');
  const worker = config.worker;
  const measurement = new Measurement(config, config.source);
  const clockKeys = ['worker.submitToStart', 'worker.resultToContinuation'].flatMap((prefix) => LATENCY_SUFFIXES.map((suffix) => `${prefix}.${suffix}`));
  const metrics = new ScenarioMetrics(measurement)
    .counters('spot.applicationHandlerEntries', 'spot.applicationYieldCalls')
    .latency('workerCallLatencyMs', 'worker.callLatency').latency('workerTaskLatencyMs', 'worker.taskLatency')
    .markUnsupported('CLOCK_DOMAIN_UNVERIFIED', 'The worker thread clock is not verified to share the main thread epoch (§15.2); this interval crosses both.', ...clockKeys)
    .markHistogramsUnsupported('CLOCK_DOMAIN_UNVERIFIED', 'The worker thread clock is not verified to share the main thread epoch (§15.2); this interval crosses both.',
      'workerSubmitToStartMs', 'workerResultToContinuationMs')
    .markUnsupported('PUBLIC_OBSERVATION_UNSUPPORTED', 'Public worker options are settings; no queue depth snapshot exists.', 'worker.pool.queueDepth.max', 'worker.pool.queueDepth.mean')
    .spotInternalsUnsupported()
    .provenance('workerOptions', {
      algorithm: worker.algorithm, taskMillis: worker.taskMillis, applied: { minThreads: worker.minThreads, maxThreads: worker.maxThreads, idleTimeoutMs: worker.idleTimeoutMs },
      callTimeoutMs: worker.workerTimeoutMs, maxQueueLength: null,
      maxQueueLengthReason: `The Node.js public ZLinkWorkerOptions has no queue length; the requested ${worker.maxQueueLength} is not applied.`
    });
  const readiness = new ObjectsReadiness(false, 'This cell has not created its User Spots yet.');
  let scenario: SpotWorkerOffloadEchoScenario | undefined;
  await runRole({
    config,
    objects: readiness,
    // §5.2: only the public worker options; the Node.js options carry no queue length.
    worker: { minThreads: worker.minThreads, maxThreads: worker.maxThreads, idleTimeoutMs: worker.idleTimeoutMs },
    providers: [{ provide: ScenarioMetrics, useValue: metrics }, SpotWorkerOffloadSpot, SpotWorkerOffloadHandler],
    configureFramework: (builder) => configureSpotRole(builder, config, false, SpotWorkerOffloadSpot),
    workload: () => scenario?.run,
    prepare: async (app) => {
      scenario = new SpotWorkerOffloadEchoScenario(app.get<ZLinkSpotOutbound>(ZLINK_SPOT_OUTBOUND, { strict: false }), app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER, { strict: false }),
        measurement, config, app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }), readiness);
      await scenario.prepare();
    }
  }, measurement);
}
