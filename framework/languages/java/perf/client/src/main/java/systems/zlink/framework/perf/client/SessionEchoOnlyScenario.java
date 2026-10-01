package systems.zlink.framework.perf.client;

import systems.zlink.framework.perf.DecimalText;
import systems.zlink.framework.perf.CompletionLoop;
import systems.zlink.framework.perf.EndpointManifest;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;

import java.net.URI;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLongArray;

// §11.1: independent CS process -> Session process; physical connector per global client ID.
// connector.request(dto).submit(PerfEchoReply.class) through full identity/byte validation is one operation.
// 1024/4096 JSON, request/ordinary, IMMEDIATE public dispatch mode; no Store or Actor.
// Server logical stream, Actor, Spot, worker and fanout metrics are not applicable.
public class SessionEchoOnlyScenario implements ClientControl.Workload {
    private record Connected(int id, ZLinkStreamConnector connector) {}
    private record SetupProbe(int local, int id, long started, ZLinkStreamConnector connector, PerfEchoRequest request,
            PerfEchoReply reply, Throwable error) {}

    private final EndpointManifest manifest;
    private final Measurement measurement;
    private final Measurement.ConnectionRange connectionRange;
    private final List<Connected> connectors = Collections.synchronizedList(new ArrayList<>());
    private final List<ZLinkStreamConnector> owned = Collections.synchronizedList(new ArrayList<>());
    private AtomicLongArray sequences = new AtomicLongArray(0);

    public SessionEchoOnlyScenario(EndpointManifest manifest, Measurement measurement, int index) {
        this.manifest = manifest;
        this.measurement = measurement;
        this.connectionRange = java.util.Objects.requireNonNull(measurement.connectionRange());
    }

    private ZLinkStreamConnector create(URI endpoint) {
        var workload = manifest.workload();
        ZLinkStreamConnectorOptions defaults = ZLinkStreamConnectorOptions.createDefault(endpoint);
        return ZLinkStreamConnectorFactory.create(new ZLinkStreamConnectorOptions(endpoint, ZLinkStreamDispatchMode.IMMEDIATE,
                Duration.ofMillis(workload.requestTimeoutMs()), defaults.waitTimeout(), defaults.maxReconnectAttempts(),
                Duration.ofMillis(workload.setupTimeoutMs()), defaults.maxSendPayloadSize(), defaults.maxReceivePayloadSize(),
                defaults.heartbeatEnabled(), defaults.heartbeatInterval(), defaults.heartbeatTimeout(),
                defaults.reconnectEnabled(), defaults.reconnectInitialDelay(), defaults.reconnectMaxDelay(),
                defaults.reconnectBackoffFactor(), defaults.skipServerCertificateValidation(), defaults.compression(),
                defaults.compressionCodec(), defaults.nameResolver(), defaults.typedCodec()));
    }

    /** Connect/setup (§4): each connector connects once and probes once; concurrency is connect-concurrency per process. */
    public CompletionStage<Void> prepare() {
        var workload = manifest.workload();
        int count = connectionRange.count();
        int first = connectionRange.first();
        sequences = new AtomicLongArray(count);
        URI endpoint = URI.create(manifest.roles().stream().filter(role -> role.streamEndpoint() != null)
                .findFirst().orElseThrow().streamEndpoint());
        Object[] evidence = new Object[count];
        AtomicInteger next = new AtomicInteger();
        AtomicInteger failures = new AtomicInteger();
        AtomicInteger successes = new AtomicInteger();
        int lanes = Math.min(workload.connectConcurrency(), Math.max(1, count));
        List<CompletableFuture<Void>> running = new ArrayList<>();
        for (int lane = 0; lane < lanes; lane++) {
            CompletableFuture<Void> done = new CompletableFuture<>();
            running.add(done);
            CompletionLoop.run(done, () -> {
                int local = next.getAndIncrement();
                if (local >= count) {
                    return Optional.empty();
                }
                return Optional.of(startSetupProbe(endpoint, first, local, evidence, failures, successes));
            });
        }
        return CompletableFuture.allOf(running.toArray(CompletableFuture[]::new)).thenRun(() -> {
            measurement.connected(successes.get());
            measurement.connectionFailures(failures.get());
            measurement.setupEvidence(List.of(evidence));
        });
    }

    private CompletionLoop.Iteration<SetupProbe> startSetupProbe(URI endpoint, int first, int local, Object[] evidence,
            AtomicInteger failures, AtomicInteger successes) {
        var workload = manifest.workload();
        int id = first + local;
        long started = PerfClock.now();
        ZLinkStreamConnector connector = null;
        PerfEchoRequest request = null;
        CompletionStage<PerfEchoReply> probe;
        try {
            connector = create(endpoint);
            owned.add(connector);
            request = measurement.request(id, sequences.incrementAndGet(local), true);
            PerfEchoRequest setupRequest = request;
            ZLinkStreamConnector setupConnector = connector;
            // Setup probe: bounded by the setup deadline; an Actor cell's probe also carries the session's create and bind.
            probe = setupConnector.connect().submit()
                    .thenCompose(ignored -> setupConnector.request(setupRequest).timeout(Duration.ofMillis(workload.setupTimeoutMs()))
                            .submit(PerfEchoReply.class))
                    .toCompletableFuture()
                    .orTimeout(workload.setupTimeoutMs(), java.util.concurrent.TimeUnit.MILLISECONDS);
            return setupIteration(local, id, started, setupConnector, setupRequest, probe, evidence, failures, successes);
        } catch (RuntimeException error) {
            return setupIteration(local, id, started, connector, request,
                    CompletableFuture.<PerfEchoReply>failedFuture(error), evidence, failures, successes);
        }
    }

    private CompletionLoop.Iteration<SetupProbe> setupIteration(int local, int id, long started,
            ZLinkStreamConnector connector, PerfEchoRequest request, CompletionStage<PerfEchoReply> probe,
            Object[] evidence, AtomicInteger failures, AtomicInteger successes) {
        CompletionStage<SetupProbe> result = probe.handle((reply, error) ->
                new SetupProbe(local, id, started, connector, request, reply, error));
        return new CompletionLoop.Iteration<>(result, (setup, completionError) -> {
            if (completionError != null) {
                setupFailed(local, id, started, completionError, evidence, failures);
                return;
            }
            Throwable failure = setup.error();
            if (failure == null) {
                try {
                    PayloadPattern.validateIdentity(setup.request(), setup.reply());
                    measurement.pattern().validate(setup.reply().payload());
                    if (!setup.connector().isConnected()) {
                        throw new IllegalStateException("Connector lost its connection during setup.");
                    }
                } catch (RuntimeException invalid) {
                    failure = invalid;
                }
            }
            if (failure == null) {
                connectors.add(new Connected(setup.id(), setup.connector()));
                successes.incrementAndGet();
                Map<String, Object> observed = new LinkedHashMap<>();
                observed.put("clientId", setup.id());
                observed.put("state", setup.connector().state().toString());
                observed.put("isConnected", setup.connector().isConnected());
                observed.put("setupLatencyNs", DecimalText.of(PerfClock.now() - setup.started()));
                observed.put("correlationId", setup.request().correlationId());
                evidence[setup.local()] = Evidence.of("connectorSetupAndTypedProbe",
                        "connect().submit + isConnected + request(dto).submit(PerfEchoReply.class)", observed);
            } else {
                setupFailed(setup.local(), setup.id(), setup.started(), failure, evidence, failures);
            }
        });
    }

    private void setupFailed(int local, int id, long started, Throwable error, Object[] evidence, AtomicInteger failures) {
        Throwable cause = Measurement.unwrap(error);
        failures.incrementAndGet();
        Map<String, Object> observed = new LinkedHashMap<>();
        observed.put("clientId", id);
        observed.put("message", cause.getMessage());
        observed.put("setupLatencyNs", DecimalText.of(PerfClock.now() - started));
        evidence[local] = Evidence.of("connectorSetupFailure", cause.getClass().getName(), observed);
    }

    /** The measured loop: every connector runs `inflight` closed-loop operation chains. */
    public CompletionStage<Void> run() {
        int first = connectionRange.first();
        List<CompletableFuture<Void>> loops = new ArrayList<>();
        List<Connected> ready;
        synchronized (connectors) {
            ready = List.copyOf(connectors);
        }
        for (Connected entry : ready) {
            for (int slot = 0; slot < manifest.workload().inflight(); slot++) {
                CompletableFuture<Void> done = new CompletableFuture<>();
                loops.add(done);
                step(entry.id(), entry.id() - first, entry.connector(), done);
            }
        }
        return CompletableFuture.allOf(loops.toArray(CompletableFuture[]::new));
    }

    private void step(int id, int local, ZLinkStreamConnector connector, CompletableFuture<Void> done) {
        CompletionLoop.run(done, () -> {
            if (!measurement.canIssue()) {
                return Optional.empty();
            }
            PerfEchoRequest request = measurement.request(id, sequences.incrementAndGet(local), false);
            long started = measurement.beginOperation();
            if (started < 0) {
                return Optional.empty();
            }
            PerfEchoRequest sent = request.withSentTicks(started);
            CompletionStage<PerfEchoReply> call;
            try {
                call = connector.request(sent).timeout(Duration.ofMillis(manifest.workload().requestTimeoutMs()))
                        .submit(PerfEchoReply.class);
            } catch (RuntimeException error) {
                call = CompletableFuture.failedFuture(error);
            }
            return Optional.of(new CompletionLoop.Iteration<>(call, (reply, error) -> {
                if (error == null) {
                    try {
                        PayloadPattern.validateIdentity(sent, reply);
                        measurement.pattern().validate(reply.payload());
                        measurement.completeOperation(started);
                    } catch (RuntimeException invalid) {
                        measurement.completeOperation(started, invalid);
                    }
                } else {
                    measurement.completeOperation(started, error);
                }
            }));
        });
    }

    @Override
    public void close() {
        List<ZLinkStreamConnector> all;
        synchronized (owned) {
            all = List.copyOf(owned);
        }
        for (ZLinkStreamConnector connector : all) {
            try {
                connector.close().submit().toCompletableFuture().get(2, java.util.concurrent.TimeUnit.SECONDS);
            } catch (Exception ignored) {
                // process teardown: a connector that does not close in time is dropped with the process
            }
        }
    }
}
