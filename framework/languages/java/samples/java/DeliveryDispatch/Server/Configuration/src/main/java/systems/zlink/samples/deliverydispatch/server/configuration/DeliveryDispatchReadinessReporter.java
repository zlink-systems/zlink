package systems.zlink.samples.deliverydispatch.server.configuration;

import java.util.Arrays;
import java.util.List;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import systems.zlink.framework.monitoring.ZLinkMeshPeerSnapshot;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import org.springframework.context.SmartLifecycle;

/** Emits sample-owned readiness evidence from the public RouteMesh monitoring surface. */
public final class DeliveryDispatchReadinessReporter implements SmartLifecycle {
    private final ZLinkRouteMeshRuntime routeMeshRuntime;
    private final String nodeName;
    private final String meshName;
    private final List<String> actorRouteTargets;
    private boolean routeReported;
    private boolean running;
    private boolean[] actorRouteReported;
    private ScheduledExecutorService executor;

    public DeliveryDispatchReadinessReporter(
        ZLinkRouteMeshRuntime routeMeshRuntime,
        String nodeName,
        String meshName,
        String... actorRouteTargets) {
        this.routeMeshRuntime = routeMeshRuntime;
        this.nodeName = nodeName;
        this.meshName = meshName;
        this.actorRouteTargets = List.copyOf(Arrays.asList(actorRouteTargets));
        this.actorRouteReported = new boolean[actorRouteTargets.length];
    }

    @Override
    public synchronized void start() {
        if (running) {
            return;
        }
        running = true;
        executor = Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, "deliverydispatch-readiness");
            thread.setDaemon(true);
            return thread;
        });
        executor.scheduleAtFixedRate(this::reportReadyState, 0, 100, TimeUnit.MILLISECONDS);
    }

    @Override
    public synchronized void stop() {
        running = false;
        if (executor != null) {
            executor.shutdownNow();
            executor = null;
        }
    }

    @Override
    public boolean isRunning() {
        return running;
    }

    @Override
    public boolean isAutoStartup() {
        return true;
    }

    @Override
    public int getPhase() {
        return 1;
    }

    private synchronized void reportReadyState() {
        if (!running || !routeMeshRuntime.isReady(meshName)) {
            return;
        }
        if (!routeReported) {
            routeReported = true;
            System.out.println("deliverydispatch-ready kind=route node=" + nodeName);
        }
        if (actorRouteTargets.isEmpty()) {
            return;
        }
        List<ZLinkMeshPeerSnapshot> peers = routeMeshRuntime.snapshot(meshName).peers();
        for (int index = 0; index < actorRouteTargets.size(); index++) {
            if (actorRouteReported[index] || !hasReadyPeer(peers, actorRouteTargets.get(index))) {
                continue;
            }
            actorRouteReported[index] = true;
            System.out.println("deliverydispatch-ready kind=actor-route node=" + nodeName
                + " target=" + actorRouteTargets.get(index));
        }
    }

    private static boolean hasReadyPeer(List<ZLinkMeshPeerSnapshot> peers, String nodeName) {
        return peers.stream().anyMatch(peer ->
            peer.nodeRid().toString().equals(nodeName) && peer.state() == ZLinkPeerState.READY);
    }
}
