package systems.zlink.framework.perf.servers.channel;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.CompletionLoop;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.Polling;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ScenarioMetrics;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.perf.SpotSetup;
import systems.zlink.framework.perf.Streams;
import systems.zlink.framework.spots.ZLinkSpotManager;

import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLongArray;

// §10.3 s2s-channel-to-spot-request-echo. Question: what a remote request to a global SpotId costs end to end (address
// lookup, delivery, reply). Roles: HTTP trigger Client x1, Channel process (Object Client, this file) x1, Spot process
// (Object Server, spot-server) x1. One operation starts at the Channel's requestToSpot and ends when the typed echo is
// validated. request, ordinary; representative payload 4096 bytes. streamId mod spotCount picks the User Spot. Store: run
// Docker Redis (automatic discovery, Spot addresses). Null: physical connections, worker, Actor, fanout; Spot mailbox and
// turn internals have no public observation.
public final class S2sChannelToSpotRequestEchoScenario {
    private final RoleConfig config;
    private final ZLinkRouteClient spots;
    private final ZLinkSpotManager manager;
    private final Measurement measurement;
    private final ZLinkRouteMeshRuntime meshRuntime;
    private final ObjectsReadiness readiness;
    private AtomicLongArray sequences = new AtomicLongArray(0);

    public S2sChannelToSpotRequestEchoScenario(ZLinkRouteClient spots, ZLinkSpotManager manager, Measurement measurement,
            ZLinkRouteMeshRuntime meshRuntime, ObjectsReadiness readiness) {
        this.config = measurement.config();
        this.spots = spots;
        this.manager = manager;
        this.measurement = measurement;
        this.meshRuntime = meshRuntime;
        this.readiness = readiness;
        new ScenarioMetrics(measurement).spotInternalsUnsupported();
    }

    public static void run(RoleConfig config) {
        if (!"channel".equals(config.role()) || !config.source()) {
            throw new IllegalArgumentException("The Channel role is the source of this scenario.");
        }
        ServerApplication app = ServerApplication.create(config)
                .configure(options -> ServerApplication.routeMesh(options, config, "perf-channel").objects().client())
                .bean(ObjectsReadiness.class, () -> new ObjectsReadiness(false, "No User Spot has been found through the public manager yet."))
                .bean(S2sChannelToSpotRequestEchoScenario.class)
                .workload(S2sChannelToSpotRequestEchoScenario.class, S2sChannelToSpotRequestEchoScenario::run);
        app.start().getBean(S2sChannelToSpotRequestEchoScenario.class).prepare();
    }

    public CompletionStage<Void> prepare() {
        int timeoutMs = config.workload().setupTimeoutMs();
        // Public status and the manager's resolve are polled; the probe call itself is never retried.
        return Polling.until(() -> {
            var mesh = meshRuntime.snapshot(config.meshName());
            return mesh.isReady() && mesh.readyPeerCount() > 0; // a ready Object Server peer, not only a ready local node
        }, 10, timeoutMs)
                .thenCompose(ignored -> SpotSetup.findAll(manager, config))
                .thenCompose(found -> {
                    sequences = new AtomicLongArray(config.workload().logicalStreams());
                    List<Object> probes = new ArrayList<>();
                    CompletableFuture<Void> chain = CompletableFuture.completedFuture(null);
                    for (int target = 0; target < config.spotIds().size(); target++) {
                        int spot = target;
                        chain = chain.thenCompose(ignored -> {
                            PerfEchoRequest request = measurement.request(spot,
                                    sequences.incrementAndGet(spot % sequences.length()), true);
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
                    return chain.thenAccept(ignored -> {
                        readiness.set(true, "", List.of(Evidence.of("spotFind", "ZLinkSpotManager.find", found))); // only after every probe
                        measurement.setupEvidence(List.of(Evidence.of("typedProbeEcho",
                                "ZLinkRouteClient.requestToSpot.submit(PerfEchoReply.class)", probes)));
                    });
                })
                .exceptionally(error -> {
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
}
