package systems.zlink.framework.perf.servers.spot;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.perf.CompletionLoop;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ScenarioMetrics;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.perf.Streams;
import systems.zlink.framework.spots.ZLinkSpotManager;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLongArray;

// §10.7 spot-no-await-echo (also the §11.3 local Spot reference). Question: what the public Spot
// client costs from the caller
// to a User Spot in the same process that echoes at once; not a pure mailbox cost. Roles: HTTP
// Client x1, Spot process (Object
// Server + local application driver, this file) x1; no Channel, Actor or worker. One operation: the
// local
// ZLinkRouteClient.requestToSpot until the typed echo is validated (codec and local dispatch
// included, the HTTP trigger is
// not). no-await: the caller is an ordinary request and the handler replies with the typed echo at
// once. Payload 1024 bytes.
// Setup: only this Object Server can place the Spots. Store: run Docker Redis (Spot addresses).
// Null: remote call, worker,
// Actor, fanout; mailbox depth and real turns have no public observation; the driver histogram is
// not kept because this
// interval is already the primary latency.
public final class SpotNoAwaitEchoScenario {
    private final RoleConfig config;
    private final ZLinkRouteClient spots;
    private final ZLinkSpotManager manager;
    private final Measurement measurement;
    private final ZLinkRouteMeshRuntime meshRuntime;
    private final ObjectsReadiness readiness;
    private AtomicLongArray sequences = new AtomicLongArray(0);

    public SpotNoAwaitEchoScenario(
            ZLinkRouteClient spots,
            ZLinkSpotManager manager,
            Measurement measurement,
            ZLinkRouteMeshRuntime meshRuntime,
            ObjectsReadiness readiness) {
        this.config = measurement.config();
        this.spots = spots;
        this.manager = manager;
        this.measurement = measurement;
        this.meshRuntime = meshRuntime;
        this.readiness = readiness;
    }

    public static void run(RoleConfig config) {
        if (!"spot".equals(config.role()) || !config.source()) {
            throw new IllegalArgumentException("The Spot role is the source of this scenario.");
        }
        ServerApplication app = SpotRole.application(config, PerfEchoSpot.class, false, null);
        app.bean(
                ScenarioMetrics.class,
                () ->
                        new ScenarioMetrics(app.measurement())
                                .counters("spot.applicationHandlerEntries")
                                .spotInternalsUnsupported());
        app.bean(SpotNoAwaitEchoScenario.class);
        app.workload(SpotNoAwaitEchoScenario.class, SpotNoAwaitEchoScenario::run);
        app.start().getBean(SpotNoAwaitEchoScenario.class).prepare();
    }

    public CompletionStage<Void> prepare() {
        return SpotRole.createSpots(config, manager, meshRuntime, measurement)
                .thenCompose(
                        objects -> {
                            if (objects == null) {
                                return CompletableFuture.<Void>completedFuture(null);
                            }
                            sequences = new AtomicLongArray(config.workload().logicalStreams());
                            List<Object> probes = new ArrayList<>();
                            CompletableFuture<Void> chain = CompletableFuture.completedFuture(null);
                            for (int target = 0; target < config.spotIds().size(); target++) {
                                int spot = target;
                                chain =
                                        chain.thenCompose(
                                                ignored -> {
                                                    PerfEchoRequest request =
                                                            measurement.request(
                                                                    spot,
                                                                    sequences.incrementAndGet(
                                                                            spot
                                                                                    % sequences
                                                                                            .length()),
                                                                    true);
                                                    return spots.requestToSpot(
                                                                    config.spotIds().get(spot),
                                                                    request)
                                                            .timeout(measurement.callTimeout())
                                                            .submit(PerfEchoReply.class)
                                                            .thenAccept(
                                                                    reply -> {
                                                                        PayloadPattern
                                                                                .validateIdentity(
                                                                                        request,
                                                                                        reply);
                                                                        measurement
                                                                                .pattern()
                                                                                .validate(
                                                                                        reply
                                                                                                .payload());
                                                                        Map<String, Object>
                                                                                observed =
                                                                                        new LinkedHashMap<>();
                                                                        observed.put(
                                                                                "correlationId",
                                                                                request
                                                                                        .correlationId());
                                                                        observed.put(
                                                                                "receivedTicks",
                                                                                reply
                                                                                        .receivedTicks());
                                                                        observed.put(
                                                                                "clockDomainId",
                                                                                reply
                                                                                        .clockDomainId());
                                                                        probes.add(observed);
                                                                    });
                                                });
                            }
                            return chain.thenAccept(
                                    done -> {
                                        SpotRole.publish(
                                                readiness,
                                                objects); // objectsReady only after every probe, so
                                        // warmup never overlaps one
                                        measurement.setupEvidence(
                                                List.of(
                                                        Evidence.of(
                                                                "typedProbeEcho",
                                                                "ZLinkRouteClient.requestToSpot.submit(PerfEchoReply.class)",
                                                                probes)));
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

    private Optional<CompletionLoop.Iteration<PerfEchoReply>> issue(int stream) {
        if (!measurement.canIssue()) {
            return Optional.empty();
        }
        String spotId = config.spotIds().get(stream % config.spotIds().size());
        PerfEchoRequest request =
                measurement.request(stream, sequences.incrementAndGet(stream), false);
        long started = measurement.beginOperation();
        if (started < 0) {
            return Optional.empty();
        }
        PerfEchoRequest sent = request.withSentTicks(started);
        CompletionStage<PerfEchoReply> call;
        try {
            call =
                    spots.requestToSpot(spotId, sent)
                            .timeout(measurement.callTimeout())
                            .submit(PerfEchoReply.class);
        } catch (RuntimeException error) {
            call = CompletableFuture.failedFuture(error);
        }
        return Optional.of(
                new CompletionLoop.Iteration<>(
                        call,
                        (reply, error) -> {
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
    }
}
