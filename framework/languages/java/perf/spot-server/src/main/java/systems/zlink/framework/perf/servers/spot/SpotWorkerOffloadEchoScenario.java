package systems.zlink.framework.perf.servers.spot;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.perf.CompletionLoop;
import systems.zlink.framework.perf.DecimalText;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.PerfValidationException;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ScenarioMetrics;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.perf.Streams;
import systems.zlink.framework.perf.WorkerObservation;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.framework.spots.ZLinkWorkerCall;
import systems.zlink.framework.spots.ZLinkWorkerCancellation;

import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLongArray;

// §10.8 spot-worker-offload-echo. Question: how much a CPU worker call and the delivery of its result add to the local Spot
// echo of §10.7. Roles: HTTP Client x1, Spot process (Object Server + local driver + Framework worker, this file) x1; no
// remote echo process. The caller is the same local ZLinkRouteClient.requestToSpot as §10.7 and its interval is the primary
// latency; the Spot handler runs one ZLinkSpotContext.runCpuWorker call per request: Yield (default) hands the turn back while
// the worker runs, ordinary keeps it. The worker callback is the self-contained xorshift32-v1 task of worker-task-millis (no
// sleep); it checks time and cancellation every 1024 iterations. Public worker options (ZLinkWorkerOptions) carry min/max
// threads and idle timeout; the call timeout is the workerTimeoutMs. worker-offload; payload 1024 bytes. Store: run Docker
// Redis (Spot addresses). Null: worker queue depth and Spot internals have no public observation; remote call, Actor, fanout
// do not apply.
public final class SpotWorkerOffloadEchoScenario {
    private final RoleConfig config;
    private final ZLinkRouteClient spots;
    private final ZLinkSpotManager manager;
    private final Measurement measurement;
    private final ScenarioMetrics metrics;
    private final WorkerSamples workerSamples;
    private final ZLinkRouteMeshRuntime meshRuntime;
    private final ObjectsReadiness readiness;
    private AtomicLongArray sequences = new AtomicLongArray(0);

    public SpotWorkerOffloadEchoScenario(ZLinkRouteClient spots, ZLinkSpotManager manager, Measurement measurement,
            ScenarioMetrics metrics, WorkerSamples workerSamples, ZLinkRouteMeshRuntime meshRuntime, ObjectsReadiness readiness) {
        this.config = measurement.config();
        this.spots = spots;
        this.manager = manager;
        this.measurement = measurement;
        this.metrics = metrics;
        this.workerSamples = workerSamples;
        this.meshRuntime = meshRuntime;
        this.readiness = readiness;
        measurement.onReset(workerSamples::clear);
    }

    public static void run(RoleConfig config) {
        if (!"spot".equals(config.role()) || !config.source() || config.worker() == null) {
            throw new IllegalArgumentException("The Spot role with worker config is the source of this scenario.");
        }
        RoleConfig.WorkerConfig worker = config.worker();
        // §5.2: only the public worker options; the Java ZLinkWorkerOptions carry no queue length.
        ServerApplication app = SpotRole.application(config, SpotWorkerOffloadSpot.class, false, options ->
                options.configureWorkers().minThreads(worker.minThreads()).maxThreads(worker.maxThreads())
                        .idleTimeout(Duration.ofMillis(worker.idleTimeoutMs())));
        Map<String, Object> applied = new LinkedHashMap<>();
        applied.put("minThreads", worker.minThreads());
        applied.put("maxThreads", worker.maxThreads());
        applied.put("idleTimeoutMs", worker.idleTimeoutMs());
        Map<String, Object> workerOptions = new LinkedHashMap<>();
        workerOptions.put("algorithm", worker.algorithm());
        workerOptions.put("taskMillis", worker.taskMillis());
        workerOptions.put("applied", applied);
        workerOptions.put("callTimeoutMs", worker.workerTimeoutMs());
        app.bean(ScenarioMetrics.class, () -> new ScenarioMetrics(app.measurement())
                .counters("spot.applicationHandlerEntries", "spot.applicationYieldCalls")
                .latency("workerCallLatencyMs", "worker.callLatency").latency("workerSubmitToStartMs", "worker.submitToStart")
                .latency("workerTaskLatencyMs", "worker.taskLatency")
                .latency("workerResultToContinuationMs", "worker.resultToContinuation")
                .unsupported("PUBLIC_OBSERVATION_UNSUPPORTED", "Public worker options are settings; no queue depth snapshot exists.",
                        "worker.pool.queueDepth.max", "worker.pool.queueDepth.mean")
                .spotInternalsUnsupported()
                .provenance("workerOptions", workerOptions));
        app.bean(WorkerSamples.class);
        app.bean(SpotWorkerOffloadEchoScenario.class);
        app.workload(SpotWorkerOffloadEchoScenario.class, SpotWorkerOffloadEchoScenario::run);
        app.start().getBean(SpotWorkerOffloadEchoScenario.class).prepare();
    }

    public CompletionStage<Void> prepare() {
        return SpotRole.createSpots(config, manager, meshRuntime, measurement).thenCompose(objects -> {
            if (objects == null) {
                return CompletableFuture.<Void>completedFuture(null);
            }
            sequences = new AtomicLongArray(config.workload().logicalStreams());
            List<Object> probes = new ArrayList<>();
            CompletableFuture<Void> chain = CompletableFuture.completedFuture(null);
            for (int target = 0; target < config.spotIds().size(); target++) {
                int spot = target;
                chain = chain.thenCompose(ignored -> {
                    PerfEchoRequest request = measurement.request(spot, sequences.incrementAndGet(spot % sequences.length()), true);
                    return spots.requestToSpot(config.spotIds().get(spot), request)
                            .timeout(Duration.ofMillis(config.workload().requestTimeoutMs()))
                            .submit(PerfEchoReply.class)
                            .thenAccept(reply -> {
                                PayloadPattern.validateIdentity(request, reply);
                                measurement.pattern().validate(reply.payload());
                                Map<String, Object> observed = new LinkedHashMap<>();
                                observed.put("correlationId", request.correlationId());
                                observed.put("receivedTicks", reply.receivedTicks());
                                observed.put("clockDomainId", reply.clockDomainId());
                                probes.add(observed);
                            });
                });
            }
            return chain.thenAccept(done -> {
                SpotRole.publish(readiness, objects); // objectsReady only after every probe, so warmup never overlaps one
                measurement.setupEvidence(List.of(Evidence.of("typedProbeEcho",
                        "ZLinkRouteClient.requestToSpot -> runCpuWorker", probes)));
            });
        }).exceptionally(error -> {
            measurement.recordDiagnostic(error);
            return null;
        });
    }

    public CompletionStage<Void> run() {
        return Streams.launch(config, this::step);
    }

    private void step(int stream, CompletableFuture<Void> done) {
        CompletionLoop.run(done, () -> {
            if (!measurement.canIssue()) {
                return Optional.empty();
            }
            String spotId = config.spotIds().get(stream % config.spotIds().size());
            PerfEchoRequest request = measurement.request(stream, sequences.incrementAndGet(stream), false);
            long started = measurement.beginOperation();
            if (started < 0) {
                return Optional.empty();
            }
            PerfEchoRequest sent = request.withSentTicks(started);
            boolean measured = "measured".equals(sent.phase());
            if (measured && !workerSamples.begin(sent.correlationId())) {
                measurement.completeOperation(started, new IllegalStateException("Worker timing correlation was already active."));
                return Optional.empty();
            }
            CompletionStage<PerfEchoReply> call;
            try {
                call = spots.requestToSpot(spotId, sent)
                        .timeout(Duration.ofMillis(config.workload().requestTimeoutMs()))
                        .submit(PerfEchoReply.class);
            } catch (RuntimeException error) {
                call = CompletableFuture.failedFuture(error);
            }
            return Optional.of(new CompletionLoop.Iteration<>(call, (reply, error) -> {
                if (error == null) {
                    try {
                        PayloadPattern.validateIdentity(sent, reply);
                        measurement.pattern().validate(reply.payload());
                        workerSamples.complete(measurement, started, sent.correlationId(), null, PerfClock.now(), metrics);
                    } catch (RuntimeException invalid) {
                        workerSamples.complete(measurement, started, sent.correlationId(), invalid, null, metrics);
                    }
                } else {
                    workerSamples.complete(measurement, started, sent.correlationId(), error, null, metrics);
                }
            }));
        });
    }

    public static final class SpotWorkerOffloadSpot extends PerfSpot {
        public SpotWorkerOffloadSpot(ZLinkSpotContext context) {
            super(context);
            context.handlers().addHandler(SpotWorkerOffloadHandler.class);
        }
    }

    // The Spot handler: one CPU worker call, then the typed echo. The worker callback returns its own timing evidence.
    public static final class SpotWorkerOffloadHandler
            implements ZLinkSpotRequestHandler<SpotWorkerOffloadSpot, PerfEchoRequest, PerfEchoReply> {
        private final Measurement measurement;
        private final ScenarioMetrics metrics;
        private final WorkerSamples workerSamples;
        private final RoleConfig config;

        public SpotWorkerOffloadHandler(Measurement measurement, ScenarioMetrics metrics, WorkerSamples workerSamples,
                RoleConfig config) {
            this.measurement = measurement;
            this.metrics = metrics;
            this.workerSamples = workerSamples;
            this.config = config;
        }

        @Override
        public CompletionStage<PerfEchoReply> handle(SpotWorkerOffloadSpot spot, PerfEchoRequest request) {
            long received = PerfClock.now();
            measurement.handlerEnter();
            try {
                measurement.validateRequest(request);
                if ("measured".equals(request.phase())) {
                    metrics.count("spot.applicationHandlerEntries");
                }
                int taskMillis = config.worker().taskMillis();
                long submitted = PerfClock.now();
                ZLinkWorkerCall<WorkerObservation> call = spot.context()
                        .runCpuWorker(worker -> xorShift32(taskMillis, worker))
                        .timeout(Duration.ofMillis(config.worker().workerTimeoutMs()));
                CompletionStage<WorkerObservation> result;
                if ("yield".equals(config.terminal())) {
                    metrics.count("spot.applicationYieldCalls");
                    result = call.yield();
                } else {
                    result = call.submit();
                }
                return result.thenApply(observation -> {
                    long resumed = PerfClock.now();
                    recordWorker(request, observation, submitted, resumed);
                    PerfEchoReply reply = PayloadPattern.reply(request, received);
                    measurement.recordReply(request);
                    if ("setup".equals(measurement.phase()) && !measurement.config().source()) {
                        Map<String, Object> observed = new LinkedHashMap<>();
                        observed.put("correlationId", request.correlationId());
                        observed.put("iterations", observation.iterations());
                        observed.put("checksum", observation.checksum());
                        measurement.setupEvidence(List.of(Evidence.of("typedProbeReply",
                                "ZLinkSpotRequestHandler -> runCpuWorker", observed)));
                    }
                    return reply;
                }).whenComplete((reply, error) -> {
                    if (error != null) {
                        measurement.recordDiagnostic(error);
                    }
                    measurement.handlerExit();
                });
            } catch (RuntimeException error) {
                measurement.recordDiagnostic(error);
                measurement.handlerExit();
                throw error;
            }
        }

        // §10.8 xorshift32-v1: x=0x12345678; x^=x<<13; x^=x>>>17; x^=x<<5 in 32 bits. Time and cancellation are checked every
        // 1024 iterations and the task ends at or after the target duration. It uses no sleep and no shared state.
        private static WorkerObservation xorShift32(int taskMillis, ZLinkWorkerCancellation cancellation) {
            long started = PerfClock.now();
            long target = started + taskMillis * 1_000_000L;
            int x = 0x12345678;
            long iterations = 0;
            do {
                for (int i = 0; i < 1024; i++) {
                    x ^= x << 13;
                    x ^= x >>> 17;
                    x ^= x << 5;
                }
                iterations += 1024;
                cancellation.throwIfCancellationRequested();
            } while (PerfClock.now() < target);
            return new WorkerObservation(DecimalText.of(started), DecimalText.of(PerfClock.now()), PerfClock.DOMAIN,
                    DecimalText.of(iterations), Integer.toUnsignedLong(x));
        }

        // The intervals of one worker call in the window. They are taken from one process clock; an observation from another
        // clock domain would be discarded rather than subtracted.
        private void recordWorker(PerfEchoRequest request, WorkerObservation observation, long submitted, long resumed) {
            if (!"measured".equals(request.phase())) {
                return;
            }
            if (!PerfClock.DOMAIN.equals(observation.clockDomainId()) || DecimalText.u64(observation.iterations()) == 0) {
                throw new PerfValidationException("SchemaMismatch", "The worker observation is not from this clock domain or is empty.");
            }
            long started = DecimalText.i64(observation.startedTicks());
            long ended = DecimalText.i64(observation.endedTicks());
            workerSamples.observe(request.correlationId(), new WorkerSamples.Intervals(submitted, started, ended, resumed));
        }
    }
}
