package systems.zlink.framework.perf;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.stream.connector.ZLinkStreamErrorCode;
import systems.zlink.stream.connector.ZLinkStreamException;

import java.lang.management.ManagementFactory;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Consumer;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

// Application cohort accounting only (§4, §13, §14). No socket state, retry, transport polling or completion pump.
// The window, reset epoch, in-flight slots and outcome counters of one process live here; every scenario reports its
// operations through beginOperation/completeOperation and reads nothing from the Framework's internals.
public final class Measurement {
    private static final ExecutorService PHASES = Executors.newCachedThreadPool(runnable -> {
        Thread thread = new Thread(runnable, "perf-phase");
        thread.setDaemon(true);
        return thread;
    });

    private final Object gate = new Object();
    private final RoleConfig config;
    private final boolean primary;
    private final ProcessSampler sampler = new ProcessSampler();
    private Histogram latency = new Histogram();
    private Histogram settleLatency = new Histogram();
    private final Map<String, Long> counts = new HashMap<>();
    private final Map<String, Long> byKind = new LinkedHashMap<>();
    private final Map<String, Long> harness = new LinkedHashMap<>();
    private final Map<String, Long> language = new LinkedHashMap<>();
    private final List<Object> errors = new ArrayList<>();
    private final List<Object> publicStateSamples = new ArrayList<>();
    private final Map<String, Long> directional = new HashMap<>();
    private long inflight;
    private long maxInflight;
    private int activeHandlers;
    private long start;
    private long end;
    private long settledAt;
    private String startUnix;
    private String endUnix;
    private String phase = "setup";
    private String resetSeq = "0";
    private boolean sealedResults;
    private ResetReply resetAck;
    private final Map<String, PerfTriggerReply> starts = new HashMap<>();
    private CompletableFuture<Void> phaseTask = CompletableFuture.completedFuture(null);
    private final PayloadPattern pattern;

    // A scenario's own counters: cleared with the window at reset, and added to every snapshot (family metrics, §14).
    private volatile Runnable onReset;
    private volatile Consumer<PerfSnapshot> enrichSnapshot;
    private volatile Supplier<Object> samplePublicState;
    private volatile boolean finalSnapshot;
    private volatile long connected;
    private volatile long connectionFailures;
    private volatile List<Object> setupEvidence = List.of();
    // The typed messages this scenario's measured path carries, one serializedMessageBytes row each (§15.2).
    private volatile List<String[]> messageTypes = List.of(
            new String[] {"request", "PerfEchoRequest"}, new String[] {"reply", "PerfEchoReply"});

    public Measurement(RoleConfig config, boolean primary) {
        this.config = config;
        this.primary = primary;
        this.pattern = new PayloadPattern(config.workload().payloadSize());
    }

    public RoleConfig config() {
        return config;
    }

    public PayloadPattern pattern() {
        return pattern;
    }

    public String phase() {
        synchronized (gate) {
            return phase;
        }
    }

    public String resetSeq() {
        synchronized (gate) {
            return resetSeq;
        }
    }

    public long endTicks() {
        synchronized (gate) {
            return end;
        }
    }

    public CompletableFuture<Void> phaseTask() {
        synchronized (gate) {
            return phaseTask;
        }
    }

    public boolean canIssue() {
        synchronized (gate) {
            return !sealedResults && start != 0 && PerfClock.now() < end;
        }
    }

    public boolean hasErrors() {
        synchronized (gate) {
            return !byKind.isEmpty() || !harness.isEmpty() || !language.isEmpty();
        }
    }

    public List<Object> errorEvidence() {
        synchronized (gate) {
            return List.copyOf(errors);
        }
    }

    public void connected(long value) {
        connected = value;
    }

    public void connectionFailures(long value) {
        connectionFailures = value;
    }

    public List<Object> setupEvidence() {
        return setupEvidence;
    }

    public void setupEvidence(List<Object> evidence) {
        setupEvidence = List.copyOf(evidence);
    }

    public void samplePublicState(Supplier<Object> sampler) {
        samplePublicState = sampler;
    }

    public void onReset(Runnable action) {
        onReset = action;
    }

    public void enrichSnapshot(Consumer<PerfSnapshot> action) {
        enrichSnapshot = action;
    }

    /** True only while the runner reads the final snapshot of a phase (§4.1: the settle ends with that read). */
    public boolean finalSnapshot() {
        return finalSnapshot;
    }

    public void finalSnapshot(boolean value) {
        finalSnapshot = value;
    }

    public void messageTypes(List<String[]> types) {
        messageTypes = types;
    }

    public PerfEchoRequest request(int stream, long sequence, boolean probe) {
        String currentReset = resetSeq();
        boolean warmup = probe || "warmup".equals(phase());
        String name = warmup ? "warmup" : "measured";
        return new PerfEchoRequest(config.runId(), config.cellId(), probe ? "0" : currentReset, name, stream,
                DecimalText.of(sequence), config.cellId() + "/" + name + "/" + stream + "/" + sequence,
                DecimalText.of(PerfClock.now()), PerfClock.DOMAIN, null, null, pattern.base64());
    }

    /** A send/send request names its return address (§10.4, §10.6); an echo request names none. */
    public void validateRequest(PerfEchoRequest request, String returnChannel, String returnSpotId) {
        if (!config.runId().equals(request.runId()) || !config.cellId().equals(request.cellId())
                || request.clientId() < 0 || !("warmup".equals(request.phase()) || "measured".equals(request.phase()))
                || !java.util.Objects.equals(request.returnSpotId(), returnSpotId)
                || !java.util.Objects.equals(request.returnChannel(), returnChannel)
                || !(request.cellId() + "/" + request.phase() + "/" + request.clientId() + "/" + request.sequence())
                        .equals(request.correlationId())
                || request.clockDomainId() == null || request.clockDomainId().isEmpty()) {
            throw new PerfValidationException("IdentityMismatch", "Request identity does not match the cell.");
        }
        DecimalText.u64(request.sequence());
        DecimalText.i64(request.sentTicks());
        long seq = DecimalText.u64(request.resetSeq());
        if ("warmup".equals(request.phase()) ? seq != 0 : seq == 0 || !request.resetSeq().equals(resetSeq())) {
            throw new PerfValidationException("PhaseMismatch", "Request reset sequence does not match the phase.");
        }
        pattern.validate(request.payload());
    }

    public void validateRequest(PerfEchoRequest request) {
        validateRequest(request, null, null);
    }

    public PerfTriggerReply start(PerfTriggerRequest trigger, Supplier<CompletionStage<Void>> workload) {
        synchronized (gate) {
            if (!config.runId().equals(trigger.runId()) || !config.cellId().equals(trigger.cellId())
                    || !("warmup".equals(trigger.phase()) || "measured".equals(trigger.phase()))
                    || !trigger.resetSeq().equals(resetSeq)
                    || ("warmup".equals(trigger.phase()) ? !"0".equals(resetSeq) : "0".equals(resetSeq))) {
                return ack(trigger, false, "rejected", "Identity, resetSeq or phase is invalid.");
            }
            String key = trigger.phase() + "/" + trigger.resetSeq();
            PerfTriggerReply previous = starts.get(key);
            if (previous != null) {
                return previous.withState("alreadyStarted");
            }
            if (!phaseTask.isDone() || inflight != 0 || activeHandlers != 0
                    || ("warmup".equals(trigger.phase()) ? !"setup".equals(phase) : !"reset".equals(phase))) {
                return ack(trigger, false, "rejected", "Previous phase has not drained and reset.");
            }
            phase = trigger.phase();
            sealedResults = false;
            start = PerfClock.now();
            double seconds = "warmup".equals(phase) ? config.workload().warmupSeconds() : config.workload().durationSeconds();
            end = Math.addExact(start, (long) (seconds * 1e9));
            startUnix = PerfClock.unixMs();
            sampler.start();
            PerfTriggerReply started = ack(trigger, true, "started", null);
            starts.put(key, started);
            // Launching on its own thread lets the HTTP/control acknowledgement leave before load starts.
            phaseTask = CompletableFuture.runAsync(() -> runPhase(workload), PHASES);
            return started;
        }
    }

    private PerfTriggerReply ack(PerfTriggerRequest trigger, boolean accepted, String state, String reason) {
        return new PerfTriggerReply(trigger.runId(), trigger.cellId(), trigger.resetSeq(), trigger.phase(), accepted,
                state, config.configHash(), reason);
    }

    private void runPhase(Supplier<CompletionStage<Void>> workload) {
        CompletionStage<Void> operations;
        try {
            operations = workload == null ? CompletableFuture.completedFuture(null) : workload.get();
        } catch (Throwable error) {
            recordDiagnostic(error);
            operations = CompletableFuture.completedFuture(null);
        }
        Supplier<Object> stateSampler = samplePublicState;
        Object initialPublicState = stateSampler == null ? null : stateSampler.get();
        if (initialPublicState != null) {
            synchronized (gate) {
                publicStateSamples.add(initialPublicState);
            }
        }
        while (true) {
            long remaining = end - PerfClock.now();
            if (remaining <= 0) {
                break;
            }
            try {
                TimeUnit.NANOSECONDS.sleep(Math.min(100_000_000L, remaining));
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                break;
            }
            // Public runtime status may marshal to a runtime lane; never call it under the counter lock.
            Object publicState = stateSampler == null ? null : stateSampler.get();
            synchronized (gate) {
                sampler.sample();
                if (publicState != null) {
                    publicStateSamples.add(publicState);
                }
            }
        }
        synchronized (gate) {
            sampler.end();
            endUnix = PerfClock.unixMs();
            phase = "settle";
        }
        try {
            long remaining = Math.max(0, end + config.workload().settleTimeoutMs() * 1_000_000L - PerfClock.now());
            operations.toCompletableFuture().get(remaining, TimeUnit.NANOSECONDS);
        } catch (TimeoutException error) {
            recordDiagnostic(new PerfValidationException("SettleIncomplete", "The settle bound elapsed with operations outstanding."));
        } catch (ExecutionException error) {
            recordDiagnostic(error.getCause() == null ? error : error.getCause());
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        } catch (RuntimeException error) {
            recordDiagnostic(error);
        }
        synchronized (gate) {
            settledAt = PerfClock.now();
            if (primary) {
                counts.put("unresolved", inflight);
                sealedResults = true;
            }
            phase = "complete";
        }
    }

    public ResetReply reset(ResetRequest request, LongSupplier resetCapacity) {
        long requested = DecimalText.u64(request.resetSeq());
        synchronized (gate) {
            boolean drained = phaseTask.isDone() && (start == 0 || PerfClock.now() >= end) && inflight == 0 && activeHandlers == 0;
            if (config.runId().equals(request.runId()) && config.cellId().equals(request.cellId()) && resetAck != null
                    && resetAck.resetSeq().equals(request.resetSeq()) && drained) {
                return resetAck;
            }
            String reason = !config.runId().equals(request.runId()) || !config.cellId().equals(request.cellId())
                    ? "Different run or cell."
                    : requested == 0 || Long.compareUnsigned(requested, DecimalText.u64(resetSeq)) <= 0
                            ? "resetSeq must advance."
                            : !drained || "setup".equals(phase)
                                    ? "Warmup or measured operations have not drained."
                                    : (!byKind.isEmpty() || !harness.isEmpty() || !language.isEmpty()) ? "Previous phase failed." : null;
            if (reason != null) {
                return new ResetReply(false, request.runId(), request.cellId(), config.role(), config.roleInstance(),
                        request.resetSeq(), PerfClock.unixMs(), null, reason, Map.of());
            }
            counts.clear();
            byKind.clear();
            harness.clear();
            language.clear();
            errors.clear();
            directional.clear();
            publicStateSamples.clear();
            latency = new Histogram();
            settleLatency = new Histogram();
            maxInflight = 0;
            start = 0;
            end = 0;
            settledAt = 0;
            startUnix = null;
            endUnix = null;
            sealedResults = false;
            Runnable scenarioReset = onReset;
            if (scenarioReset != null) {
                scenarioReset.run();
            }
            resetSeq = request.resetSeq();
            phase = "reset";
            String resetAt = PerfClock.unixMs();
            Long epoch = resetCapacity == null ? null : resetCapacity.getAsLong();
            Map<String, NullReason> reasons = new LinkedHashMap<>();
            if (epoch == null) {
                reasons.put("/capacityEpoch", new NullReason("NOT_APPLICABLE", "The client owns no Framework host."));
            }
            resetAck = new ResetReply(true, config.runId(), config.cellId(), config.role(), config.roleInstance(),
                    resetSeq, resetAt, epoch == null ? null : DecimalText.of(epoch), null, reasons);
            return resetAck;
        }
    }

    /** Starts one measured logical operation; returns its start ticks, or -1 when the window is closed. */
    public long beginOperation(String direction) {
        synchronized (gate) {
            long started = PerfClock.now();
            if (sealedResults || start == 0 || started >= end) {
                return -1;
            }
            increment(counts, "sent");
            inflight = Math.addExact(inflight, 1);
            maxInflight = Math.max(maxInflight, inflight);
            increment(directional, direction);
            return started;
        }
    }

    public long beginOperation() {
        return beginOperation("request");
    }

    /** A send/send echo keeps the time it was observed even when the first send's terminal comes later (§13). */
    public void completeOperation(long started, Throwable error, Long completedTicks) {
        long completed = completedTicks == null ? PerfClock.now() : completedTicks;
        synchronized (gate) {
            if (sealedResults) {
                return;
            }
            inflight--;
            if (error == null) {
                boolean inWindow = completed < end;
                increment(counts, inWindow ? "completed" : "settleCompleted");
                (inWindow ? latency : settleLatency).record(completed - started);
            } else {
                recordError(error, true);
            }
        }
    }

    public void completeOperation(long started) {
        completeOperation(started, null, null);
    }

    public void completeOperation(long started, Throwable error) {
        completeOperation(started, error, null);
    }

    public void handlerEnter() {
        synchronized (gate) {
            activeHandlers++;
        }
    }

    public void handlerExit() {
        synchronized (gate) {
            activeHandlers--;
        }
    }

    public void recordReply(PerfEchoRequest request) {
        recordApplicationCall(request, "reply");
    }

    /** A public call this process starts (send) or a typed reply it returns, counted once inside its own window. */
    public void recordApplicationCall(PerfEchoRequest request, String direction) {
        synchronized (gate) {
            if (request.resetSeq().equals(resetSeq) && start != 0 && PerfClock.now() < end
                    && request.phase().equals("0".equals(resetSeq) ? "warmup" : "measured")) {
                increment(directional, direction);
            }
        }
    }

    public void recordDiagnostic(Throwable error) {
        synchronized (gate) {
            recordError(error, false);
        }
    }

    /** The public exception behind a completion stage's wrapper. */
    public static Throwable unwrap(Throwable error) {
        Throwable current = error;
        while ((current instanceof CompletionException || current instanceof ExecutionException) && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static String kindName(ZLinkFrameworkErrorKind kind) {
        StringBuilder name = new StringBuilder();
        for (String part : kind.name().split("_")) {
            name.append(part.charAt(0)).append(part.substring(1).toLowerCase(java.util.Locale.ROOT));
        }
        return name.toString();
    }

    private void recordError(Throwable raw, boolean outcome) {
        Throwable error = unwrap(raw);
        String category = "failed";
        String publicKind = null;
        String harnessKind = null;
        String connectorCode = null;
        if (error instanceof ZLinkFrameworkException framework) {
            // The public enum exactly matches the common error kind names (§14.2).
            publicKind = kindName(framework.kind());
            increment(byKind, publicKind);
            if (framework.kind() == ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED) {
                category = "timeout";
            }
        } else if (error instanceof PerfValidationException validation) {
            harnessKind = validation.kind();
            increment(harness, harnessKind);
            if ("CorrelationExpired".equals(harnessKind)) {
                category = "timeout"; // §13: an expired correlation is a timeout
            }
        } else if (error instanceof ZLinkStreamException connector) {
            connectorCode = connector.errorCode().name();
            increment(language, connector.getClass().getName());
            if (connector.errorCode() == ZLinkStreamErrorCode.REQUEST_TIMEOUT) {
                category = "timeout";
            }
        } else {
            increment(language, error.getClass().getName());
            if (error instanceof CancellationException) {
                category = "cancelled";
            } else if (error instanceof TimeoutException) {
                category = "timeout";
            }
        }
        if (outcome) {
            increment(counts, category);
        }
        if (errors.size() < 32) {
            Map<String, Object> evidence = new LinkedHashMap<>();
            evidence.put("type", error.getClass().getName());
            evidence.put("message", error.getMessage());
            evidence.put("publicKind", publicKind);
            evidence.put("harnessKind", harnessKind);
            evidence.put("connectorCode", connectorCode);
            errors.add(evidence);
        }
    }

    private static void increment(Map<String, Long> values, String key) {
        values.merge(key, 1L, (a, b) -> Math.addExact(a, b));
    }

    private static Map<String, Object> texts(Map<String, Long> values) {
        Map<String, Object> result = new LinkedHashMap<>();
        values.forEach((key, value) -> result.put(key, DecimalText.of(value)));
        return result;
    }

    private static long get(Map<String, Long> values, String key) {
        return values.getOrDefault(key, 0L);
    }

    public PerfSnapshot snapshot(Object publicStatus) {
        synchronized (gate) {
            Map<String, Object> metrics = new LinkedHashMap<>();
            Map<String, Object> histograms = new LinkedHashMap<>();
            Map<String, Object> runtime = new LinkedHashMap<>();
            Map<String, NullReason> reasons = new LinkedHashMap<>();
            MetricCatalog.baselineNulls(metrics, histograms, reasons);
            for (String key : MetricCatalog.OUTCOMES) {
                if (primary) {
                    metrics.put("messages." + key, DecimalText.of("unresolved".equals(key) && !sealedResults ? inflight : get(counts, key)));
                } else {
                    MetricCatalog.nullValue(metrics, reasons, "metrics", "messages." + key, "NOT_APPLICABLE",
                            "Echo outcomes belong to the source process.");
                }
            }
            if (primary) {
                latency.export("latencyMs", "latency", metrics, histograms, reasons);
                settleLatency.export("settleLatencyMs", "settle.latency", metrics, histograms, reasons);
            } else {
                for (String prefix : List.of("latency", "settle.latency")) {
                    for (String suffix : MetricCatalog.LATENCY_SUFFIXES) {
                        MetricCatalog.nullValue(metrics, reasons, "metrics", prefix + "." + suffix, "NOT_APPLICABLE",
                                "RTT belongs to the source process.");
                    }
                }
                for (String key : List.of("latencyMs", "settleLatencyMs")) {
                    MetricCatalog.nullValue(histograms, reasons, "histograms", key, "NOT_APPLICABLE",
                            "RTT belongs to the source process.");
                }
            }
            RoleConfig.Workload workload = config.workload();
            boolean csClient = "client".equals(config.role()) && workload.connections() != null;
            for (String key : List.of("requested", "connected", "failed")) {
                if (csClient) {
                    long value = switch (key) {
                        case "requested" -> workload.connections() / workload.clientCount()
                                + (config.roleInstance() < workload.connections() % workload.clientCount() ? 1 : 0);
                        case "connected" -> connected;
                        default -> connectionFailures;
                    };
                    metrics.put("connections." + key, DecimalText.of(value));
                } else {
                    MetricCatalog.nullValue(metrics, reasons, "metrics", "connections." + key, "NOT_APPLICABLE",
                            "This process owns no physical connector pool.");
                }
            }
            for (String key : List.of("logicalStreams", "inflightPerStream", "inflight.max")) {
                if (primary && (!"logicalStreams".equals(key) || !csClient)) {
                    long value = switch (key) {
                        case "logicalStreams" -> workload.logicalStreams();
                        case "inflightPerStream" -> workload.inflight();
                        default -> maxInflight;
                    };
                    metrics.put("load." + key, DecimalText.of(value));
                } else {
                    MetricCatalog.nullValue(metrics, reasons, "metrics", "load." + key, "NOT_APPLICABLE",
                            "No server logical streams are owned here; CS slots are connector based.");
                }
            }
            for (String direction : List.of("request", "send", "reply", "event")) {
                long count = get(directional, direction);
                metrics.put("applicationMessages." + direction, DecimalText.of(count));
                metrics.put("applicationPayloadBytes." + direction, DecimalText.of(Math.multiplyExact(count, (long) workload.payloadSize())));
            }
            Double seconds = start == 0 ? null : (end - start) / 1e9;
            long applicationCount = 0;
            for (long value : directional.values()) {
                applicationCount = Math.addExact(applicationCount, value);
            }
            metrics.put("throughput.kops", primary && seconds != null && seconds > 0 ? get(counts, "completed") / seconds / 1000 : null);
            metrics.put("throughput.messagesPerSec", seconds != null && seconds > 0 ? applicationCount / seconds : null);
            metrics.put("throughput.megabytesPerSec",
                    seconds != null && seconds > 0 ? applicationCount * (double) workload.payloadSize() / seconds / 1048576 : null);
            metrics.put("errors.byKind", texts(byKind));
            metrics.put("errors.harness", texts(harness));
            metrics.put("errors.language", texts(language));
            for (String key : List.of("throughput.kops", "throughput.messagesPerSec", "throughput.megabytesPerSec")) {
                if (metrics.get(key) == null) {
                    reasons.put("/metrics/" + key, new NullReason(start == 0 ? "PHASE_NOT_STARTED" : "NOT_APPLICABLE",
                            "No applicable completed measurement window."));
                }
            }
            if ("complete".equals(phase)) {
                sampler.export(metrics, runtime, reasons);
            } else {
                for (String key : List.of("process.cpuPercent", "process.rssMb", "process.allocatedMb", "gc.gen0", "gc.gen1", "gc.gen2")) {
                    MetricCatalog.nullValue(metrics, reasons, "metrics", key, "PHASE_NOT_STARTED",
                            "Process window sampling has not completed.");
                }
            }
            runtime.put("setupEvidence", ProcessSampler.named("setupEvidence", "observation", "array", setupEvidence));
            runtime.put("publicReadinessSamples", ProcessSampler.named("public host readiness and pressure samples",
                    "observation", "array", List.copyOf(publicStateSamples)));
            runtime.put("errors", ProcessSampler.named("firstErrors", "observation", "array", List.copyOf(errors)));
            runtime.put("activeHandlers", ProcessSampler.named("application active handlers", "count", "integer",
                    DecimalText.of(activeHandlers)));
            Map<String, Object> window = new LinkedHashMap<>();
            window.put("startedAtUnixMs", startUnix);
            window.put("endedAtUnixMs", endUnix);
            window.put("startTicks", start == 0 ? null : DecimalText.of(start));
            window.put("endTicks", end == 0 ? null : DecimalText.of(end));
            window.put("measuredSeconds", seconds);
            window.put("settleSeconds", settledAt == 0 ? null : Math.max(0, settledAt - end) / 1e9);
            window.forEach((key, value) -> {
                if (value == null) {
                    reasons.put("/window/" + key, new NullReason("PHASE_NOT_STARTED", "Window or settle has not completed."));
                }
            });
            for (String key : List.of("alignmentMethod", "maxErrorNs", "validFromTicks", "validThroughTicks")) {
                reasons.put("/clock/" + key, new NullReason("NOT_APPLICABLE", "RTT uses the caller process clock only."));
            }
            if (publicStatus == null) {
                reasons.put("/publicStatus", new NullReason("NOT_APPLICABLE", "The client has no Framework host runtime."));
            }
            List<Map<String, Object>> serialized = new ArrayList<>();
            int index = 0;
            for (String[] type : messageTypes) {
                Map<String, Object> row = new LinkedHashMap<>();
                row.put("direction", type[0]);
                row.put("packetName", type[1]);
                row.put("logicalPayloadBytes", DecimalText.of(workload.payloadSize()));
                row.put("observedSerializedBytes", null);
                serialized.add(row);
                reasons.put("/serializedMessageBytes/" + index + "/observedSerializedBytes", new NullReason(
                        "PUBLIC_OBSERVATION_UNSUPPORTED",
                        "No public per-DTO serialized byte observation; the measured message is serialized once by the Framework."));
                index++;
            }
            Map<String, Object> provenance = new LinkedHashMap<>(config.provenance());
            provenance.put("pid", ProcessHandle.current().pid());
            provenance.put("host", hostName());
            provenance.put("messageCountScope", "application-call-boundaries");
            provenance.put("configHash", config.configHash());
            provenance.put("resetAcknowledgement", resetAck);
            provenance.put("primaryEchoOwner", primary);
            provenance.put("runtimeVersion", System.getProperty("java.version"));
            provenance.put("effectiveProcessorCount", Runtime.getRuntime().availableProcessors());
            Map<String, Object> executor = new LinkedHashMap<>();
            executor.put("name", "JVM");
            executor.put("vm", ManagementFactory.getRuntimeMXBean().getVmName());
            executor.put("availableProcessors", Runtime.getRuntime().availableProcessors());
            executor.put("commonPoolParallelism", ForkJoinPool.commonPool().getParallelism());
            executor.put("maxHeapBytes", DecimalText.of(Runtime.getRuntime().maxMemory()));
            executor.put("liveThreadCount", ManagementFactory.getThreadMXBean().getThreadCount());
            executor.put("inputArguments", ManagementFactory.getRuntimeMXBean().getInputArguments());
            provenance.put("executor", executor);
            PerfSnapshot snapshot = new PerfSnapshot(config.runId(), config.cellId(), resetSeq, config.role(),
                    config.roleInstance(), config.configHash(), phase, window, PerfClock.metadata(), serialized,
                    metrics, histograms, reasons, publicStatus, runtime, provenance);
            Consumer<PerfSnapshot> enrich = enrichSnapshot;
            if (enrich != null) {
                enrich.accept(snapshot);
            }
            return snapshot;
        }
    }

    private static String hostName() {
        try {
            return InetAddress.getLocalHost().getHostName();
        } catch (UnknownHostException error) {
            return "unknown";
        }
    }

}
