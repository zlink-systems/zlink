package systems.zlink.framework.perf.servers.spot;

import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.perf.CompletionLoop;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfDriveReply;
import systems.zlink.framework.perf.PerfDriveRequest;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.PerfValidationException;
import systems.zlink.framework.perf.Polling;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ScenarioMetrics;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.perf.Streams;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLongArray;

// §10.5 s2s-spot-to-channel-request-echo. Question: how the terminal (ordinary or Yield) and the
// number of Spots change
// completion throughput, tail latency and the progress of other callbacks for the same Spot ->
// Channel remote request.
// Roles: HTTP Client x1, Spot process (Object Server + local public driver, this file) x1, Channel
// echo target x1. The driver
// sends PerfDriveRequest to the Spot with ZLinkRouteClient.requestToSpot; the Spot handler (an
// Actor-less SpotWide User Spot)
// makes one requestToChannel. The measured operation starts right before that remote call and ends
// after the reply is
// validated (after the turn is regained); driver time is kept apart as driver.latency.*. Streams
// are assigned to SpotIds
// round-robin, so no Actor queue takes part.
// request; ordinary or yield x 1 or 16 Spots; payload 4096 bytes. Store: run Docker Redis (Spot
// addresses and automatic
// mesh). Null: physical connections, worker, Actor, fanout; suspended/resumed turns, resume latency
// and mailbox depth have no
// public observation. Yield calls are the application's calls, not proven turn suspensions.
public final class S2sSpotToChannelRequestEchoScenario {
    private final RoleConfig config;
    private final ZLinkRouteClient spots;
    private final ZLinkSpotManager manager;
    private final Measurement measurement;
    private final ZLinkRouteMeshRuntime meshRuntime;
    private final ObjectsReadiness readiness;
    private final ScenarioMetrics metrics;
    private AtomicLongArray sequences = new AtomicLongArray(0);

    public S2sSpotToChannelRequestEchoScenario(
            ZLinkRouteClient spots,
            ZLinkSpotManager manager,
            Measurement measurement,
            ZLinkRouteMeshRuntime meshRuntime,
            ObjectsReadiness readiness,
            ScenarioMetrics metrics) {
        this.config = measurement.config();
        this.spots = spots;
        this.manager = manager;
        this.measurement = measurement;
        this.meshRuntime = meshRuntime;
        this.readiness = readiness;
        this.metrics = metrics;
    }

    public static void run(RoleConfig config) {
        if (!"spot".equals(config.role()) || !config.source()) {
            throw new IllegalArgumentException("The Spot role is the source of this scenario.");
        }
        ServerApplication app =
                SpotRole.application(config, S2sRemoteRequestSpot.class, true, null);
        app.bean(
                ScenarioMetrics.class,
                () ->
                        new ScenarioMetrics(app.measurement())
                                .counters(
                                        "driver.issued",
                                        "driver.notStarted",
                                        "driver.failed",
                                        "spot.applicationHandlerEntries",
                                        "spot.applicationYieldCalls")
                                .latency("driverLatencyMs", "driver.latency")
                                .aliasLatency("latency", "spot.remoteCallLatency")
                                .spotInternalsUnsupported());
        app.bean(S2sSpotToChannelRequestEchoScenario.class);
        app.workload(
                S2sSpotToChannelRequestEchoScenario.class,
                S2sSpotToChannelRequestEchoScenario::run);
        app.start().getBean(S2sSpotToChannelRequestEchoScenario.class).prepare();
    }

    public CompletionStage<Void> prepare() {
        return SpotRole.createSpots(config, manager, meshRuntime, measurement)
                .thenCompose(
                        objects -> {
                            if (objects == null) {
                                return CompletableFuture.<Void>completedFuture(null);
                            }
                            return Polling.until(
                                            () ->
                                                    meshRuntime
                                                            .snapshot(config.meshName())
                                                            .channels()
                                                            .stream()
                                                            .anyMatch(
                                                                    channel ->
                                                                            channel.channelName()
                                                                                            .equals(
                                                                                                    config
                                                                                                            .channelName())
                                                                                    && channel
                                                                                            .isReady()
                                                                                    && channel
                                                                                                    .readyTargetCount()
                                                                                            > 0),
                                            10,
                                            config.workload().setupTimeoutMs())
                                    .thenCompose(
                                            ready -> {
                                                sequences =
                                                        new AtomicLongArray(
                                                                config.workload().logicalStreams());
                                                List<Object> probes = new ArrayList<>();
                                                CompletableFuture<Void> chain =
                                                        CompletableFuture.completedFuture(null);
                                                for (int target = 0;
                                                        target < config.spotIds().size();
                                                        target++) {
                                                    int spot = target;
                                                    chain =
                                                            chain.thenCompose(
                                                                    ignored -> {
                                                                        PerfEchoRequest echo =
                                                                                measurement.request(
                                                                                        spot,
                                                                                        sequences
                                                                                                .incrementAndGet(
                                                                                                        spot
                                                                                                                % sequences
                                                                                                                        .length()),
                                                                                        true);
                                                                        return spots.requestToSpot(
                                                                                        config.spotIds()
                                                                                                .get(
                                                                                                        spot),
                                                                                        new PerfDriveRequest(
                                                                                                echo))
                                                                                .timeout(
                                                                                        measurement
                                                                                                .callTimeout(
                                                                                                        true))
                                                                                .submit(
                                                                                        PerfDriveReply
                                                                                                .class)
                                                                                .thenAccept(
                                                                                        driven -> {
                                                                                            if (!driven
                                                                                                            .started()
                                                                                                    || driven
                                                                                                                    .echo()
                                                                                                            == null) {
                                                                                                throw new PerfValidationException(
                                                                                                        "IdentityMismatch",
                                                                                                        "The setup"
                                                                                                                + " probe"
                                                                                                                + " did not"
                                                                                                                + " reach"
                                                                                                                + " the Channel.");
                                                                                            }
                                                                                            Map<
                                                                                                            String,
                                                                                                            Object>
                                                                                                    observed =
                                                                                                            new LinkedHashMap<>();
                                                                                            observed
                                                                                                    .put(
                                                                                                            "correlationId",
                                                                                                            echo
                                                                                                                    .correlationId());
                                                                                            observed
                                                                                                    .put(
                                                                                                            "receivedTicks",
                                                                                                            driven.echo()
                                                                                                                    .receivedTicks());
                                                                                            observed
                                                                                                    .put(
                                                                                                            "clockDomainId",
                                                                                                            driven.echo()
                                                                                                                    .clockDomainId());
                                                                                            probes
                                                                                                    .add(
                                                                                                            observed);
                                                                                        });
                                                                    });
                                                }
                                                return chain.thenAccept(
                                                        done -> {
                                                            SpotRole.publish(
                                                                    readiness,
                                                                    objects); // objectsReady only
                                                            // after every probe,
                                                            // so warmup never
                                                            // overlaps one
                                                            measurement.setupEvidence(
                                                                    List.of(
                                                                            Evidence.of(
                                                                                    "typedProbeEcho",
                                                                                    "ZLinkRouteClient.requestToSpot"
                                                                                            + " -> Spot"
                                                                                            + " requestToChannel",
                                                                                    probes)));
                                                        });
                                            });
                        })
                .exceptionally(
                        error -> {
                            measurement.recordDiagnostic(error);
                            return null;
                        });
    }

    public CompletionStage<Void> run() {
        return Streams.launchTerminals(config, measurement, this::issue);
    }

    // Each local stream starts its next request after the previous driver reply.
    private Optional<CompletionLoop.Iteration<PerfDriveReply>> issue(int stream) {
        if (!measurement.canIssue()) {
            return Optional.empty();
        }
        String spotId = config.spotIds().get(stream % config.spotIds().size());
        PerfEchoRequest echo =
                measurement.request(stream, sequences.incrementAndGet(stream), false);
        long started = PerfClock.now();
        metrics.count("driver.issued");
        CompletionStage<PerfDriveReply> call;
        try {
            call =
                    spots.requestToSpot(spotId, new PerfDriveRequest(echo))
                            .timeout(measurement.callTimeout(true))
                            .submit(PerfDriveReply.class);
        } catch (RuntimeException error) {
            call = CompletableFuture.failedFuture(error);
        }
        return Optional.of(
                new CompletionLoop.Iteration<>(
                        call,
                        (driven, error) -> {
                            if (error != null) {
                                driverFailed(error);
                            } else if (driven.started()) {
                                if (driven.echo() != null) {
                                    metrics.record("driverLatencyMs", started, PerfClock.now());
                                }
                            } else {
                                metrics.count("driver.notStarted");
                            }
                        }));
    }

    private void driverFailed(Throwable error) {
        metrics.count("driver.failed");
        measurement.recordDiagnostic(error);
    }

    public static final class S2sRemoteRequestSpot extends PerfSpot {
        public S2sRemoteRequestSpot(ZLinkSpotContext context) {
            super(context);
            context.handlers().addHandler(S2sRemoteRequestDriveHandler.class);
        }
    }

    // The Spot handler: one requestToChannel per drive request. Ordinary keeps the Spot turn until
    // the reply; Yield hands the
    // turn back while the reply is pending (execution gate contract). This is the measured
    // operation.
    public static final class S2sRemoteRequestDriveHandler
            implements ZLinkSpotRequestHandler<
                    S2sRemoteRequestSpot, PerfDriveRequest, PerfDriveReply> {
        private final Measurement measurement;
        private final ScenarioMetrics metrics;
        private final RoleConfig config;

        public S2sRemoteRequestDriveHandler(
                Measurement measurement, ScenarioMetrics metrics, RoleConfig config) {
            this.measurement = measurement;
            this.metrics = metrics;
            this.config = config;
        }

        @Override
        public CompletionStage<PerfDriveReply> handle(
                S2sRemoteRequestSpot spot, PerfDriveRequest drive) {
            measurement.handlerEnter();
            try {
                PerfEchoRequest request = drive.echo();
                measurement.validateRequest(request);
                if ("measured".equals(request.phase())) {
                    metrics.count("spot.applicationHandlerEntries");
                }
                boolean probe =
                        "setup".equals(measurement.phase()); // the setup probe is no measured
                // operation
                long started = PerfClock.now();
                if (!probe) {
                    started = measurement.beginOperation();
                    if (started < 0) {
                        measurement.handlerExit();
                        return CompletableFuture.completedFuture(new PerfDriveReply(false, null));
                    }
                }
                try {
                    long operationStarted = started;
                    PerfEchoRequest sent = request.withSentTicks(operationStarted);
                    ZLinkRequestCall call =
                            spot.context()
                                    .outbound()
                                    .requestToChannel(config.channelName(), sent)
                                    .timeout(measurement.callTimeout());
                    CompletionStage<PerfEchoReply> reply;
                    if ("yield".equals(config.terminal())) {
                        metrics.count("spot.applicationYieldCalls");
                        reply = call.yield(PerfEchoReply.class);
                    } else {
                        reply = call.submit(PerfEchoReply.class);
                    }
                    return reply.handle(
                            (echoed, error) -> {
                                try {
                                    if (error == null) {
                                        try {
                                            PayloadPattern.validateIdentity(sent, echoed);
                                            measurement.pattern().validate(echoed.payload());
                                        } catch (RuntimeException invalid) {
                                            return failed(probe, operationStarted, invalid);
                                        }
                                        if (!probe) {
                                            measurement.completeOperation(
                                                    operationStarted, null, PerfClock.now());
                                        }
                                        return new PerfDriveReply(true, echoed);
                                    }
                                    return failed(probe, operationStarted, error);
                                } finally {
                                    measurement.handlerExit();
                                }
                            });
                } catch (RuntimeException error) {
                    if (!probe) {
                        measurement.completeOperation(started, error);
                    }
                    throw error;
                }
            } catch (RuntimeException error) {
                measurement.handlerExit();
                throw error;
            }
        }

        private PerfDriveReply failed(boolean probe, long started, Throwable error) {
            if (probe) {
                throw new CompletionException(error);
            }
            measurement.completeOperation(started, error);
            return new PerfDriveReply(true, null);
        }
    }
}
