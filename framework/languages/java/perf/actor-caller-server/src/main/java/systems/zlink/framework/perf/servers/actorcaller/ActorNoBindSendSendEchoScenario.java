package systems.zlink.framework.perf.servers.actorcaller;

import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.perf.CompletionLoop;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Lanes;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ScenarioMetrics;
import systems.zlink.framework.perf.SendSendCorrelation;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.perf.Streams;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLongArray;

// §10.10 actor-no-bind-send-send-echo. Question: what do the source admission of a global-ActorId
// send and the application
// echo round trip cost, measured apart? Roles: HTTP Client x1 (trigger only), ActorCaller (Object
// Client plus the Server of a
// run/cell-only return ChannelName, this file) x1, Actor (Object Server) x1.
// One operation: the correlation is registered and sendToActor starts; it ends when the return
// Channel handler validates the
// echo (§13). Separately, actor.sourceAdmission.* is the same call's start to the sendToActor
// terminal.
// send-send, ordinary; 4096 bytes; one unbound Actor per logical stream. Store: run Docker Redis.
// Null: physical connections, Spot, worker, fanout; the remote mailbox acceptance time is not
// publicly observable.
public final class ActorNoBindSendSendEchoScenario {
    private final RoleConfig config;
    private final ZLinkActorClient actorClient;
    private final Measurement measurement;
    private final ActorCallerSetup setup;
    private final ObjectsReadiness readiness;
    private final SendSendCorrelation correlations;
    private final ScenarioMetrics metrics;
    private AtomicLongArray sequences = new AtomicLongArray(0);

    public ActorNoBindSendSendEchoScenario(
            ZLinkActorClient actorClient,
            Measurement measurement,
            ActorCallerSetup setup,
            ObjectsReadiness readiness,
            SendSendCorrelation correlations,
            ScenarioMetrics metrics) {
        this.config = measurement.config();
        this.actorClient = actorClient;
        this.measurement = measurement;
        this.setup = setup;
        this.readiness = readiness;
        this.correlations = correlations;
        this.metrics = metrics;
    }

    public static void run(RoleConfig config) {
        ServerApplication app =
                ServerApplication.create(config)
                        .configure(
                                options -> {
                                    var mesh =
                                            ServerApplication.routeMesh(
                                                    options, config, "perf-actor-caller");
                                    mesh.objects().client();
                                    mesh.channelName(config.channelName())
                                            .server()
                                            .addSendHandler(
                                                    ActorReturnHandler.class, PerfEchoReply.class);
                                });
        app.bean(
                ObjectsReadiness.class,
                () ->
                        new ObjectsReadiness(
                                false,
                                "Actors are not yet created and probed through the public API."));
        app.bean(
                ScenarioMetrics.class,
                () ->
                        new ScenarioMetrics(app.measurement())
                                .latency("sourceAdmissionMs", "actor.sourceAdmission.latency"));
        app.bean(SendSendCorrelation.class);
        app.bean(ActorCallerSetup.class);
        app.bean(ActorNoBindSendSendEchoScenario.class);
        app.workload(ActorNoBindSendSendEchoScenario.class, ActorNoBindSendSendEchoScenario::run);
        app.start().getBean(ActorNoBindSendSendEchoScenario.class).prepare();
    }

    public CompletionStage<Void> prepare() {
        return setup.createActors()
                .thenCompose(
                        created -> {
                            sequences = new AtomicLongArray(config.workload().logicalStreams());
                            // §5: probes per prepared target, bounded by connect-concurrency.
                            return Lanes.forEach(
                                            sequences.length(),
                                            config.workload().connectConcurrency(),
                                            stream -> {
                                                PerfEchoRequest request =
                                                        measurement
                                                                .request(
                                                                        stream,
                                                                        sequences.incrementAndGet(
                                                                                stream),
                                                                        true)
                                                                .withReturnChannel(
                                                                        config.channelName());
                                                SendSendCorrelation.Entry entry =
                                                        correlations.register(
                                                                request, PerfClock.now());
                                                return actorClient
                                                        .sendToActor(
                                                                config.actorIds().get(stream),
                                                                request)
                                                        .submit()
                                                        .thenCompose(
                                                                sentOk ->
                                                                        correlations.completeAsync(
                                                                                entry))
                                                        .thenAccept(
                                                                result -> {
                                                                    if (result.error() != null) {
                                                                        throw new java.util
                                                                                .concurrent
                                                                                .CompletionException(
                                                                                result.error());
                                                                    }
                                                                });
                                            })
                                    .thenAccept(
                                            done -> {
                                                Map<String, Object> observed =
                                                        new LinkedHashMap<>();
                                                observed.put("probes", sequences.length());
                                                observed.put("streams", sequences.length());
                                                measurement.setupEvidence(
                                                        List.of(
                                                                Evidence.of(
                                                                        "typedProbeEcho",
                                                                        "ZLinkActorClient.sendToActor"
                                                                                + " -> return Channel"
                                                                                + " send handler",
                                                                        observed)));
                                                // §16.1: objectsReady means the create and the
                                                // probe echo of every Actor are done, so warmup
                                                // starts on quiet roles.
                                                List<Object> evidence = new ArrayList<>();
                                                evidence.add(created);
                                                evidence.addAll(measurement.setupEvidence());
                                                readiness.set(true, "", evidence);
                                            });
                        })
                .exceptionally(
                        error -> {
                            measurement.recordDiagnostic(error);
                            return null;
                        });
    }

    public CompletionStage<Void> run() {
        return Streams.launch(config, measurement, this::step);
    }

    private void step(int stream, CompletableFuture<Void> done) {
        CompletionLoop.run(
                done,
                () -> {
                    if (!measurement.canIssue()) {
                        return Optional.empty();
                    }
                    String actorId = config.actorIds().get(stream);
                    PerfEchoRequest request =
                            measurement
                                    .request(stream, sequences.incrementAndGet(stream), false)
                                    .withReturnChannel(config.channelName());
                    long started = measurement.beginOperation("send");
                    if (started < 0) {
                        return Optional.empty();
                    }
                    PerfEchoRequest sent = request.withSentTicks(started);
                    SendSendCorrelation.Entry entry;
                    try {
                        entry =
                                correlations.register(
                                        sent,
                                        started); // §13: register immediately before the first
                        // public send
                    } catch (RuntimeException error) {
                        measurement.completeOperation(started, error);
                        return Optional.empty();
                    }
                    CompletionStage<Void> first;
                    try {
                        first = actorClient.sendToActor(actorId, sent).submit();
                    } catch (RuntimeException error) {
                        first = CompletableFuture.failedFuture(error);
                    }
                    CompletionStage<Void> admission =
                            first.handle(
                                    (ignored, error) -> {
                                        if (error == null) {
                                            metrics.record(
                                                    "sourceAdmissionMs", started, PerfClock.now());
                                        }
                                        correlations.firstSendEnded(entry, error);
                                        return null;
                                    });
                    correlations
                            .completeAsync(entry)
                            .whenComplete(
                                    (result, error) -> {
                                        if (error != null) {
                                            measurement.completeOperation(started, error);
                                            measurement.recordDiagnostic(error);
                                        } else {
                                            measurement.completeOperation(
                                                    started,
                                                    result.error(),
                                                    result.completedTicks());
                                        }
                                    });
                    return Optional.of(
                            new CompletionLoop.Iteration<>(admission, (ignored, error) -> {}));
                });
    }
}
