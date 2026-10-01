package systems.zlink.framework.perf.servers.channel;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.perf.CompletionLoop;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.Polling;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ScenarioMetrics;
import systems.zlink.framework.perf.SendSendCorrelation;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.perf.SpotSetup;
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

// §10.4 s2s-channel-to-spot-send-send-echo. Question: how completion rate, throughput and round trip differ from the
// request form of §10.3 when both directions are one-way sends. Roles: HTTP Client x1, Channel process (Object Client plus
// the Server of a run/cell-only return ChannelName, this file) x1, Spot process (Object Server) x1.
// One operation: the correlation is registered and the first sendToSpot starts; it ends when the return Channel handler
// validates the echo (§13). send-send, ordinary; payload 4096 bytes; streamId mod spotCount picks the Spot.
// Store: run Docker Redis. Null: physical connections, worker, Actor, fanout; Spot internals are not observable.
public final class S2sChannelToSpotSendSendEchoScenario {
    private final RoleConfig config;
    private final ZLinkRouteClient spots;
    private final ZLinkSpotManager manager;
    private final Measurement measurement;
    private final ZLinkRouteMeshRuntime meshRuntime;
    private final ObjectsReadiness readiness;
    private final SendSendCorrelation correlations;
    private AtomicLongArray sequences = new AtomicLongArray(0);

    public S2sChannelToSpotSendSendEchoScenario(ZLinkRouteClient spots, ZLinkSpotManager manager, Measurement measurement,
            ZLinkRouteMeshRuntime meshRuntime, ObjectsReadiness readiness, SendSendCorrelation correlations) {
        this.config = measurement.config();
        this.spots = spots;
        this.manager = manager;
        this.measurement = measurement;
        this.meshRuntime = meshRuntime;
        this.readiness = readiness;
        this.correlations = correlations;
    }

    public static void run(RoleConfig config) {
        if (!"channel".equals(config.role()) || !config.source()) {
            throw new IllegalArgumentException("The Channel role is the source of this scenario.");
        }
        ServerApplication app = ServerApplication.create(config).configure(options -> {
            var mesh = ServerApplication.routeMesh(options, config, "perf-channel");
            mesh.objects().client();
            mesh.channelName(config.channelName()).server().addSendHandler(S2sReturnHandler.class, PerfEchoReply.class);
        });
        app.bean(ObjectsReadiness.class, () -> new ObjectsReadiness(false, "No User Spot has been found through the public manager yet."));
        app.bean(ScenarioMetrics.class, () -> new ScenarioMetrics(app.measurement()).spotInternalsUnsupported());
        app.bean(SendSendCorrelation.class);
        app.bean(S2sChannelToSpotSendSendEchoScenario.class);
        app.workload(S2sChannelToSpotSendSendEchoScenario.class, S2sChannelToSpotSendSendEchoScenario::run);
        app.start().getBean(S2sChannelToSpotSendSendEchoScenario.class).prepare();
    }

    public CompletionStage<Void> prepare() {
        int timeoutMs = config.workload().setupTimeoutMs();
        return Polling.until(() -> meshRuntime.snapshot(config.meshName()).isReady(), 10, timeoutMs)
                .thenCompose(ignored -> SpotSetup.findAll(manager, config))
                .thenCompose(found -> {
                    sequences = new AtomicLongArray(config.workload().logicalStreams());
                    List<Object> probes = new ArrayList<>();
                    CompletableFuture<Void> chain = CompletableFuture.completedFuture(null);
                    for (int target = 0; target < config.spotIds().size(); target++) {
                        int spot = target;
                        chain = chain.thenCompose(ignored -> {
                            PerfEchoRequest request = measurement.request(spot,
                                    sequences.incrementAndGet(spot % sequences.length()), true)
                                    .withReturnChannel(config.channelName());
                            SendSendCorrelation.Entry entry = correlations.register(request, systems.zlink.framework.perf.PerfClock.now());
                            return spots.sendToSpot(config.spotIds().get(spot), request).submit()
                                    .thenCompose(sentOk -> correlations.completeAsync(entry))
                                    .thenAccept(result -> {
                                        if (result.error() != null) {
                                            throw new java.util.concurrent.CompletionException(result.error());
                                        }
                                        Map<String, Object> observed = new LinkedHashMap<>();
                                        observed.put("correlationId", request.correlationId());
                                        probes.add(observed);
                                    });
                        });
                    }
                    return chain.thenAccept(ignored -> {
                        readiness.set(true, "", List.of(Evidence.of("spotFind", "ZLinkSpotManager.find", found))); // only after every probe
                        measurement.setupEvidence(List.of(Evidence.of("typedProbeEcho",
                                "ZLinkRouteClient.sendToSpot -> return Channel send handler", probes)));
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
            PerfEchoRequest request = measurement.request(stream, sequences.incrementAndGet(stream), false)
                    .withReturnChannel(config.channelName());
            long started = measurement.beginOperation("send");
            if (started < 0) {
                return Optional.empty();
            }
            PerfEchoRequest sent = request.withSentTicks(started);
            SendSendCorrelation.Entry entry;
            try {
                entry = correlations.register(sent, started); // §13: register immediately before the first public send
            } catch (RuntimeException error) {
                measurement.completeOperation(started, error);
                return Optional.empty();
            }
            CompletionStage<Void> first;
            try {
                first = spots.sendToSpot(spotId, sent).submit();
            } catch (RuntimeException error) {
                first = CompletableFuture.failedFuture(error);
            }
            CompletionStage<SendSendCorrelation.Result> operation = first.handle((ignored, error) -> {
                correlations.firstSendEnded(entry, error);
                return correlations.completeAsync(entry);
            }).thenCompose(result -> result);
            return Optional.of(new CompletionLoop.Iteration<>(operation, (result, error) -> {
                if (error != null) {
                    measurement.completeOperation(started, error);
                    measurement.recordDiagnostic(error);
                } else {
                    measurement.completeOperation(started, result.error(), result.completedTicks());
                }
            }));
        });
    }
}
