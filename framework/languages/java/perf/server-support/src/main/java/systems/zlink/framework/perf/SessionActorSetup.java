package systems.zlink.framework.perf;

import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;

import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// The Session role's create and bind (§10.1, §10.2 preparation): the connector's setup probe names its clientId, which
// selects the Actor ID of that connector; the Actor is created through the public manager and bound to the session before
// the probe itself is relayed. Setup latencies are kept apart from the measured operations.
public final class SessionActorSetup {
    private final Object gate = new Object();
    private final RoleConfig config;
    private final Measurement measurement;
    private final ObjectsReadiness readiness;
    private final ZLinkActorManager actors;
    private final ZLinkRouteMeshRuntime mesh;
    private long created;
    private long existing;
    private long bound;
    private long failed;
    private long createNs;
    private long createMaxNs;
    private long bindNs;
    private long bindMaxNs;
    // Public status shows when the Actor node is a ready peer; every session shares this one wait, and the create itself
    // is never retried.
    private CompletableFuture<Void> peer;

    public SessionActorSetup(RoleConfig config, Measurement measurement, ObjectsReadiness readiness,
            ZLinkActorManager actors, ZLinkRouteMeshRuntime mesh) {
        this.config = config;
        this.measurement = measurement;
        this.readiness = readiness;
        this.actors = actors;
        this.mesh = mesh;
    }

    private CompletableFuture<Void> waitForPeer() {
        synchronized (gate) {
            if (peer == null) {
                peer = "ObjectClient".equals(config.objectRole())
                        ? Polling.until(() -> mesh.snapshot(config.meshName()).readyPeerCount() > 0, 10,
                                config.workload().setupTimeoutMs())
                        : CompletableFuture.completedFuture(null);
            }
            return peer;
        }
    }

    public CompletionStage<ZLinkSessionActor> prepare(ZLinkSessionContext session, ZLinkMessage probe) {
        if (!"setup".equals(measurement.phase())) {
            return CompletableFuture.failedFuture(new IllegalStateException("Actors are created and bound during setup only."));
        }
        try {
            PerfEchoRequest request = probe.decode(PerfEchoRequest.class);
            if (request.clientId() < 0 || request.clientId() >= config.actorIds().size()) {
                throw new PerfValidationException("IdentityMismatch", "clientId has no Actor ID in this cell.");
            }
            Duration setupTimeout = Duration.ofMillis(config.workload().setupTimeoutMs());
            long[] marks = new long[3];
            return waitForPeer().thenCompose(ignored -> {
                marks[0] = PerfClock.now();
                return actors.getOrCreate(config.actorIds().get(request.clientId()), PerfActorType.NAME)
                        .inMesh(config.meshName()).timeout(setupTimeout).submit();
            }).thenCompose(result -> {
                ActorRef actor;
                boolean wasCreated;
                if (result instanceof ZLinkActorCreateResult.Created value) {
                    actor = value.actor();
                    wasCreated = true;
                } else if (result instanceof ZLinkActorCreateResult.Existing value) {
                    actor = value.actor();
                    wasCreated = false;
                } else {
                    throw new IllegalStateException("Actor creation was rejected.");
                }
                marks[1] = PerfClock.now();
                return session.actors().bindOrGet(actor).thenApply(binding -> {
                    marks[2] = PerfClock.now();
                    record(wasCreated, marks[1] - marks[0], marks[2] - marks[1]);
                    return binding;
                });
            }).whenComplete((binding, error) -> {
                if (error != null) {
                    measurement.recordDiagnostic(error);
                    synchronized (gate) {
                        failed++;
                    }
                    publish();
                }
            });
        } catch (RuntimeException error) {
            measurement.recordDiagnostic(error);
            synchronized (gate) {
                failed++;
            }
            publish();
            return CompletableFuture.failedFuture(error);
        }
    }

    private void record(boolean wasCreated, long create, long bind) {
        synchronized (gate) {
            if (wasCreated) {
                created++;
            } else {
                existing++;
            }
            bound++;
            createNs += create;
            createMaxNs = Math.max(createMaxNs, create);
            bindNs += bind;
            bindMaxNs = Math.max(bindMaxNs, bind);
        }
        publish();
    }

    private void publish() {
        synchronized (gate) {
            Map<String, Object> observed = new LinkedHashMap<>();
            observed.put("created", created);
            observed.put("existing", existing);
            observed.put("bound", bound);
            observed.put("failed", failed);
            observed.put("expectedActors", config.actorIds().size());
            observed.put("createMeanMs", bound == 0 ? 0 : createNs / 1e6 / bound);
            observed.put("createMaxMs", createMaxNs / 1e6);
            observed.put("bindMeanMs", bound == 0 ? 0 : bindNs / 1e6 / bound);
            observed.put("bindMaxMs", bindMaxNs / 1e6);
            readiness.set(bound > 0 && failed == 0, failed > 0 ? "Actor create or bind failed." : "No Actor is bound to a session yet.",
                    List.of(Evidence.of("actorCreateAndBind",
                            "ZLinkActorManager.getOrCreate + ZLinkSessionActors.bindOrGet", observed)));
            // The Session role has no typed reply of its own: its setup probe is the admitted relay of a bound Actor.
            if (bound > 0) {
                measurement.setupEvidence(List.of(Evidence.of("relayAdmission", "ZLinkSessionActor.relay", Map.of("bound", bound))));
            }
        }
    }
}
