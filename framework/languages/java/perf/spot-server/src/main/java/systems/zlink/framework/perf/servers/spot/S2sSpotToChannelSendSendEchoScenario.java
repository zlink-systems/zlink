package systems.zlink.framework.perf.servers.spot;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.perf.CompletionLoop;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfDriveReply;
import systems.zlink.framework.perf.PerfDriveRequest;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.PerfValidationException;
import systems.zlink.framework.perf.Polling;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ScenarioMetrics;
import systems.zlink.framework.perf.SendSendCorrelation;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.perf.Streams;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLongArray;

// §10.6 s2s-spot-to-channel-send-send-echo. Question: what a send from a Spot to a Channel that
// comes back to the original
// Spot as a separate send costs in completion rate and time. Roles: HTTP Client x1, Spot process
// (Object Server + local public
// driver, this file) x1, Channel target (Object Client) x1. The driver sends PerfDriveRequest to
// the Spot with
// ZLinkRouteClient.requestToSpot; the Spot handler registers the correlation, makes the first
// sendToChannel and returns once
// that send is admitted, so the turn is free when the Channel's send comes back to the Spot's
// return handler. The driver,
// outside the turn, waits for the correlation and keeps the in-flight slot until the echo is
// validated (§13). One operation:
// correlation registration / first send -> return handler echo validation. The DTO's returnSpotId
// names the source User
// SpotId. send-send; ordinary; payload 4096 bytes. Store: run Docker Redis. Null: physical
// connections, worker, Actor,
// fanout; Spot internals are not observable.
public final class S2sSpotToChannelSendSendEchoScenario {
    private final RoleConfig config;
    private final ZLinkRouteClient spots;
    private final ZLinkSpotManager manager;
    private final Measurement measurement;
    private final ZLinkRouteMeshRuntime meshRuntime;
    private final ObjectsReadiness readiness;
    private final ScenarioMetrics metrics;
    private final SendSendCorrelation correlations;
    private AtomicLongArray sequences = new AtomicLongArray(0);

    public S2sSpotToChannelSendSendEchoScenario(
            ZLinkRouteClient spots,
            ZLinkSpotManager manager,
            Measurement measurement,
            ZLinkRouteMeshRuntime meshRuntime,
            ObjectsReadiness readiness,
            ScenarioMetrics metrics,
            SendSendCorrelation correlations) {
        this.config = measurement.config();
        this.spots = spots;
        this.manager = manager;
        this.measurement = measurement;
        this.meshRuntime = meshRuntime;
        this.readiness = readiness;
        this.metrics = metrics;
        this.correlations = correlations;
    }

    public static void run(RoleConfig config) {
        if (!"spot".equals(config.role()) || !config.source()) {
            throw new IllegalArgumentException("The Spot role is the source of this scenario.");
        }
        ServerApplication app = SpotRole.application(config, S2sSendSendSpot.class, true, null);
        app.bean(
                ScenarioMetrics.class,
                () ->
                        new ScenarioMetrics(app.measurement())
                                .counters(
                                        "driver.issued",
                                        "driver.notStarted",
                                        "driver.failed",
                                        "spot.applicationHandlerEntries")
                                .latency("driverLatencyMs", "driver.latency")
                                .spotInternalsUnsupported());
        app.bean(SendSendCorrelation.class);
        app.bean(S2sSpotToChannelSendSendEchoScenario.class);
        app.workload(
                S2sSpotToChannelSendSendEchoScenario.class,
                S2sSpotToChannelSendSendEchoScenario::run);
        app.start().getBean(S2sSpotToChannelSendSendEchoScenario.class).prepare();
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
                                                                                measurement
                                                                                        .request(
                                                                                                spot,
                                                                                                sequences
                                                                                                        .incrementAndGet(
                                                                                                                spot
                                                                                                                        % sequences
                                                                                                                                .length()),
                                                                                                true)
                                                                                        .withReturnSpotId(
                                                                                                config.spotIds()
                                                                                                        .get(
                                                                                                                spot));
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
                                                                                .thenCompose(
                                                                                        driven -> {
                                                                                            if (!driven
                                                                                                    .started()) {
                                                                                                throw new PerfValidationException(
                                                                                                        "IdentityMismatch",
                                                                                                        "The setup"
                                                                                                                + " probe"
                                                                                                                + " was not"
                                                                                                                + " started.");
                                                                                            }
                                                                                            SendSendCorrelation
                                                                                                            .Entry
                                                                                                    entry =
                                                                                                            correlations
                                                                                                                    .find(
                                                                                                                            echo
                                                                                                                                    .correlationId());
                                                                                            if (entry
                                                                                                    == null) {
                                                                                                throw new PerfValidationException(
                                                                                                        "UnknownCorrelation",
                                                                                                        "The setup"
                                                                                                                + " probe"
                                                                                                                + " registered"
                                                                                                                + " no correlation.");
                                                                                            }
                                                                                            return correlations
                                                                                                    .completeAsync(
                                                                                                            entry);
                                                                                        })
                                                                                .thenAccept(
                                                                                        result -> {
                                                                                            if (result
                                                                                                            .error()
                                                                                                    != null) {
                                                                                                throw new java
                                                                                                        .util
                                                                                                        .concurrent
                                                                                                        .CompletionException(
                                                                                                        result
                                                                                                                .error());
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
                                                                                    "Spot sendToChannel"
                                                                                            + " -> Channel"
                                                                                            + " sendToSpot"
                                                                                            + " -> Spot"
                                                                                            + " return"
                                                                                            + " handler",
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
        return Streams.launchRequests(config, measurement, this::issue);
    }

    private Optional<CompletionLoop.Iteration<Void>> issue(int stream) {
        if (!measurement.canIssue()) {
            return Optional.empty();
        }
        String spotId = config.spotIds().get(stream % config.spotIds().size());
        PerfEchoRequest echo =
                measurement
                        .request(stream, sequences.incrementAndGet(stream), false)
                        .withReturnSpotId(spotId);
        long driverStarted = PerfClock.now();
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
        CompletionStage<Void> operation =
                call.handle(
                                (driven, error) -> {
                                    if (error != null) {
                                        driverFailed(error);
                                        return completeCorrelation(echo, driverStarted, 0, false);
                                    } else if (!driven.started()) {
                                        metrics.count("driver.notStarted");
                                        return CompletableFuture.<Void>completedFuture(null);
                                    } else {
                                        // Outside the Spot turn: the final result of the
                                        // correlation the handler registered (§13).
                                        return completeCorrelation(
                                                echo, driverStarted, PerfClock.now(), true);
                                    }
                                })
                        .thenCompose(result -> result);
        return Optional.of(
                new CompletionLoop.Iteration<>(
                        operation,
                        (ignored, error) -> {
                            if (error != null) {
                                measurement.recordDiagnostic(error);
                            }
                        }));
    }

    CompletionStage<Void> completeCorrelation(
            PerfEchoRequest echo,
            long driverStarted,
            long driverCompleted,
            boolean driverSucceeded) {
        SendSendCorrelation.Entry entry = correlations.find(echo.correlationId());
        if (entry == null) {
            if (driverSucceeded) {
                measurement.recordDiagnostic(
                        new PerfValidationException(
                                "UnknownCorrelation",
                                "The started drive registered no correlation."));
            }
            return CompletableFuture.completedFuture(null);
        }
        return correlations
                .completeAsync(entry)
                .thenAccept(
                        result -> {
                            boolean counted =
                                    measurement.completeOperation(
                                            entry.startedTicks(),
                                            result.error(),
                                            result.completedTicks());
                            if (driverSucceeded && counted) {
                                metrics.record("driverLatencyMs", driverStarted, driverCompleted);
                            }
                        });
    }

    void driverFailed(Throwable error) {
        metrics.count("driver.failed");
        measurement.recordDiagnostic(error);
    }

    public static final class S2sSendSendSpot extends PerfSpot {
        public S2sSendSendSpot(ZLinkSpotContext context) {
            super(context);
            context.handlers().addHandler(S2sSendDriveHandler.class);
            context.handlers().addHandler(S2sSendReturnHandler.class);
        }
    }

    // The source Spot handler: register the correlation, make the first public send and return when
    // it is admitted.
    public static final class S2sSendDriveHandler
            implements ZLinkSpotRequestHandler<S2sSendSendSpot, PerfDriveRequest, PerfDriveReply> {
        private final Measurement measurement;
        private final ScenarioMetrics metrics;
        private final SendSendCorrelation correlations;
        private final RoleConfig config;

        public S2sSendDriveHandler(
                Measurement measurement,
                ScenarioMetrics metrics,
                SendSendCorrelation correlations,
                RoleConfig config) {
            this.measurement = measurement;
            this.metrics = metrics;
            this.correlations = correlations;
            this.config = config;
        }

        @Override
        public CompletionStage<PerfDriveReply> handle(
                S2sSendSendSpot spot, PerfDriveRequest drive) {
            measurement.handlerEnter();
            try {
                PerfEchoRequest request = drive.echo();
                measurement.validateRequest(request, null, spot.context().spotId());
                if (request.returnSpotId() == null || request.returnSpotId().isEmpty()) {
                    throw new PerfValidationException(
                            "IdentityMismatch", "No return SpotId in the request.");
                }
                if ("measured".equals(request.phase())) {
                    metrics.count("spot.applicationHandlerEntries");
                }
                boolean probe =
                        "setup".equals(measurement.phase()); // the setup probe is no measured
                // operation
                long started = PerfClock.now();
                if (!probe) {
                    started = measurement.beginOperation("send");
                    if (started < 0) {
                        measurement.handlerExit();
                        return CompletableFuture.completedFuture(new PerfDriveReply(false, null));
                    }
                }
                PerfEchoRequest sent = request.withSentTicks(started);
                SendSendCorrelation.Entry entry;
                try {
                    entry =
                            correlations.register(
                                    sent,
                                    started); // §13: registered right before the first public send
                } catch (RuntimeException error) {
                    // The operation started but cannot be tied to a correlation: it ends here as a
                    // failure.
                    if (!probe) {
                        measurement.completeOperation(started, error);
                    }
                    throw error;
                }
                CompletionStage<Void> first;
                try {
                    first =
                            spot.context()
                                    .outbound()
                                    .sendToChannel(config.channelName(), sent)
                                    .submit();
                } catch (RuntimeException error) {
                    correlations.firstSendEnded(entry, error);
                    measurement.handlerExit();
                    return CompletableFuture.completedFuture(new PerfDriveReply(true, null));
                }
                // send/send: the reply is only the first send's acknowledgement.
                return first.handle(
                        (ignored, error) -> {
                            correlations.firstSendEnded(entry, error);
                            measurement.handlerExit();
                            return new PerfDriveReply(true, null);
                        });
            } catch (RuntimeException error) {
                measurement.handlerExit();
                throw error;
            }
        }
    }

    // The return send arrives as its own Spot packet: the correlation decides the operation's first
    // result.
    public static final class S2sSendReturnHandler
            implements ZLinkSpotPacketHandler<S2sSendSendSpot, PerfEchoReply> {
        private final Measurement measurement;
        private final ScenarioMetrics metrics;
        private final SendSendCorrelation correlations;

        public S2sSendReturnHandler(
                Measurement measurement,
                ScenarioMetrics metrics,
                SendSendCorrelation correlations) {
            this.measurement = measurement;
            this.metrics = metrics;
            this.correlations = correlations;
        }

        @Override
        public CompletionStage<Void> handle(S2sSendSendSpot spot, PerfEchoReply message) {
            measurement.handlerEnter();
            try {
                if ("measured".equals(message.phase())) {
                    metrics.count("spot.applicationHandlerEntries");
                }
                correlations.reply(message);
                return CompletableFuture.completedFuture(null);
            } finally {
                measurement.handlerExit();
            }
        }
    }
}
