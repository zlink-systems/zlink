package systems.zlink.framework.perf.servers.actorcaller;

import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.perf.Lanes;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.CompletionLoop;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Streams;

import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLongArray;

// §10.9 actor-no-bind-request-echo. Question: what do address lookup and a remote request cost when a global ActorId is
// reached without any Session binding? Roles: HTTP Client x1 (trigger only), ActorCaller (Object Client, this file) x1,
// Actor (Object Server) x1. One operation: requestToActor starts and ends when the typed echo has been validated. request,
// ordinary; 4096 bytes; one unbound Actor per logical stream, created during setup.
// Store: run Docker Redis. Null: physical connections, Spot, worker, fanout and actor.sourceAdmission (no send).
public final class ActorNoBindRequestEchoScenario {
    private final RoleConfig config;
    private final ZLinkActorClient actorClient;
    private final Measurement measurement;
    private final ActorCallerSetup setup;
    private final ObjectsReadiness readiness;
    private AtomicLongArray sequences = new AtomicLongArray(0);

    public ActorNoBindRequestEchoScenario(ZLinkActorClient actorClient, Measurement measurement, ActorCallerSetup setup,
            ObjectsReadiness readiness) {
        this.config = measurement.config();
        this.actorClient = actorClient;
        this.measurement = measurement;
        this.setup = setup;
        this.readiness = readiness;
    }

    public static void run(RoleConfig config) {
        ServerApplication app = ServerApplication.create(config)
                .configure(options -> ServerApplication.routeMesh(options, config, "perf-actor-caller").objects().client())
                .bean(ObjectsReadiness.class, () -> new ObjectsReadiness(false, "Actors are not yet created and probed through the public API."))
                .bean(ActorCallerSetup.class)
                .bean(ActorNoBindRequestEchoScenario.class)
                .workload(ActorNoBindRequestEchoScenario.class, ActorNoBindRequestEchoScenario::run);
        app.start().getBean(ActorNoBindRequestEchoScenario.class).prepare();
    }

    public CompletionStage<Void> prepare() {
        return setup.createActors().thenCompose(created -> {
            sequences = new AtomicLongArray(config.workload().logicalStreams());
            // §5: probes per prepared target, bounded by connect-concurrency.
            return Lanes.forEach(sequences.length(), config.workload().connectConcurrency(), stream -> {
                PerfEchoRequest request = measurement.request(stream, sequences.incrementAndGet(stream), true);
                return actorClient.requestToActor(config.actorIds().get(stream), request)
                        .timeout(Duration.ofMillis(config.workload().setupTimeoutMs()))
                        .submit(PerfEchoReply.class)
                        .thenAccept(reply -> {
                            PayloadPattern.validateIdentity(request, reply);
                            measurement.pattern().validate(reply.payload());
                        });
            }).thenAccept(done -> {
                Map<String, Object> observed = new LinkedHashMap<>();
                observed.put("probes", sequences.length());
                observed.put("streams", sequences.length());
                measurement.setupEvidence(List.of(Evidence.of("typedProbeEcho",
                        "ZLinkActorClient.requestToActor.submit(PerfEchoReply.class)", observed)));
                // §16.1: objectsReady means the create and the probe echo of every Actor are done, so warmup starts on quiet roles.
                List<Object> evidence = new ArrayList<>();
                evidence.add(created);
                evidence.addAll(measurement.setupEvidence());
                readiness.set(true, "", evidence);
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
            String actorId = config.actorIds().get(stream);
            PerfEchoRequest request = measurement.request(stream, sequences.incrementAndGet(stream), false);
            long started = measurement.beginOperation();
            if (started < 0) {
                return Optional.empty();
            }
            PerfEchoRequest sent = request.withSentTicks(started);
            CompletionStage<PerfEchoReply> call;
            try {
                call = actorClient.requestToActor(actorId, sent)
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
