package systems.zlink.framework.perf;

import systems.zlink.framework.monitoring.ZLinkPlacementSnapshot;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// The Actor role's objectsReady (§16.1): the Actors this process hosts, read from the public
// RouteMesh placement
// status by a background poll during setup, never from inside a handler turn.
public final class ActorPlacementWatcher {
    private final RoleConfig config;
    private final Measurement measurement;
    private final ObjectsReadiness readiness;
    private final ZLinkRouteMeshRuntime mesh;

    public ActorPlacementWatcher(
            RoleConfig config,
            Measurement measurement,
            ObjectsReadiness readiness,
            ZLinkRouteMeshRuntime mesh) {
        this.config = config;
        this.measurement = measurement;
        this.readiness = readiness;
        this.mesh = mesh;
    }

    public void start() {
        Thread thread = new Thread(this::run, "perf-actor-placement");
        thread.setDaemon(true);
        thread.start();
    }

    private void run() {
        while (!Thread.currentThread().isInterrupted() && "setup".equals(measurement.phase())) {
            ZLinkPlacementSnapshot placement = mesh.snapshot(config.meshName()).placement();
            Map<String, Object> observed = new LinkedHashMap<>();
            observed.put("isAvailable", placement.isAvailable());
            observed.put("activeActorCount", placement.activeActorCount());
            int expected = config.actorIds().size();
            observed.put("expectedActors", expected);
            boolean ready = placement.isAvailable() && placement.activeActorCount() == expected;
            readiness.set(
                    ready,
                    ready
                            ? ""
                            : placement.activeActorCount()
                                    + " of "
                                    + expected
                                    + " expected Actors are active on this Object Server.",
                    List.of(
                            Evidence.of(
                                    "actorPlacement",
                                    "ZLinkRouteMeshRuntime.snapshot.placement",
                                    observed)));
            try {
                Thread.sleep(100);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            }
        }
    }
}
