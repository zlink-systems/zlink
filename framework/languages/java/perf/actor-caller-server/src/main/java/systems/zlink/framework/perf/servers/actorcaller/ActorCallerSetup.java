package systems.zlink.framework.perf.servers.actorcaller;

import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Lanes;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PerfActorType;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.Polling;
import systems.zlink.framework.perf.RoleConfig;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLong;

// §10.9 and §10.10 preparation: one unbound Actor per logical stream, created through the public
// manager during setup (§4).
// The create is never retried; only the public RouteMesh status says when the Actor node is a ready
// peer.
public final class ActorCallerSetup {
    private final RoleConfig config;
    private final Measurement measurement;
    private final ZLinkActorManager actors;
    private final ZLinkRouteMeshRuntime mesh;

    public ActorCallerSetup(
            Measurement measurement, ZLinkActorManager actors, ZLinkRouteMeshRuntime mesh) {
        this.config = measurement.config();
        this.measurement = measurement;
        this.actors = actors;
        this.mesh = mesh;
    }

    /**
     * Completes with the create result as evidence; the caller reports objectsReady once its probes
     * have also finished.
     */
    public CompletionStage<Object> createActors() {
        AtomicLong created = new AtomicLong();
        AtomicLong existing = new AtomicLong();
        AtomicLong totalNs = new AtomicLong();
        AtomicLong maxNs = new AtomicLong();
        return Polling.until(
                        () -> mesh.snapshot(config.meshName()).readyPeerCount() > 0,
                        10,
                        config.workload().setupTimeoutMs())
                .thenCompose(
                        ignored ->
                                Lanes.forEach(
                                        config.actorIds().size(),
                                        config.workload().connectConcurrency(),
                                        index -> {
                                            long started = PerfClock.now();
                                            return actors.getOrCreate(
                                                            config.actorIds().get(index),
                                                            PerfActorType.NAME)
                                                    .inMesh(config.meshName())
                                                    .timeout(measurement.callTimeout())
                                                    .submit()
                                                    .thenAccept(
                                                            result -> {
                                                                long elapsed =
                                                                        PerfClock.now() - started;
                                                                if (result
                                                                        instanceof
                                                                        ZLinkActorCreateResult
                                                                                .Created) {
                                                                    created.incrementAndGet();
                                                                } else if (result
                                                                        instanceof
                                                                        ZLinkActorCreateResult
                                                                                .Existing) {
                                                                    existing.incrementAndGet();
                                                                } else {
                                                                    throw new IllegalStateException(
                                                                            "Actor '"
                                                                                    + config.actorIds()
                                                                                            .get(
                                                                                                    index)
                                                                                    + "' creation"
                                                                                    + " was rejected.");
                                                                }
                                                                totalNs.addAndGet(elapsed);
                                                                maxNs.accumulateAndGet(
                                                                        elapsed, Math::max);
                                                            });
                                        }))
                .thenApply(
                        ignored -> {
                            Map<String, Object> observed = new LinkedHashMap<>();
                            observed.put("created", created.get());
                            observed.put("existing", existing.get());
                            observed.put("expectedActors", config.actorIds().size());
                            observed.put(
                                    "createMeanMs", totalNs.get() / 1e6 / config.actorIds().size());
                            observed.put("createMaxMs", maxNs.get() / 1e6);
                            return Evidence.of(
                                    "actorCreate",
                                    "ZLinkActorManager.getOrCreate.inMesh.submit",
                                    observed);
                        });
    }
}
