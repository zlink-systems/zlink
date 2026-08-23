package systems.zlink.samples.zoneworld.server.ops;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.Flow;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import org.springframework.boot.ApplicationArguments;
import org.springframework.boot.ApplicationRunner;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkMeshPeerSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.samples.zoneworld.server.configuration.NodeRegistry;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
/**
 * Whether a zone node is registered and its link is up. Neither is a question the console
 * can ask: a node that has stopped is not there to answer, so the console learns it from
 * the mesh runtime it already observes in its own process.
 */
public final class NodeLivenessObserver implements ApplicationRunner {
    private final ZLinkRouteMeshRuntime runtime;
    private final NodeRegistry registry;
    // The runtime signals its observation publisher through a weak reference, so the
    // stream stops as soon as the publisher is collected. The console watches node
    // lifecycles for as long as it runs, so it keeps the publisher for that long.
    private Flow.Publisher<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> observation;
    private final ScheduledExecutorService expiry = Executors.newSingleThreadScheduledExecutor(
        runnable -> { Thread thread = new Thread(runnable, "zoneworld-report-expiry");
            thread.setDaemon(true); return thread; });

    public NodeLivenessObserver(ZLinkRouteMeshRuntime runtime, NodeRegistry registry) {
        this.runtime = runtime;
        this.registry = registry;
    }

    @Override
    public void run(ApplicationArguments args) {
        observation = runtime.observe(ZoneWorldNames.MESH, 32);
        observation.subscribe(
            new Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>>() {
                @Override
                public void onSubscribe(Flow.Subscription subscription) {
                    subscription.request(Long.MAX_VALUE);
                }

                @Override
                public void onNext(ZLinkObservedStatus<ZLinkMeshNodeSnapshot> observed) {
                    registry.applyLiveRoutingIds(readyRoutingIds(observed.status()));
                }

                @Override
                public void onError(Throwable error) {
                    System.out.println("node observation error mesh=" + ZoneWorldNames.MESH
                        + " detail=" + error.getMessage());
                }

                @Override
                public void onComplete() {
                }
            });
        expiry.scheduleAtFixedRate(registry::expireStaleReports, 1, 1, TimeUnit.SECONDS);
    }

    private static Map<String, Boolean> readyRoutingIds(ZLinkMeshNodeSnapshot status) {
        Map<String, Boolean> observed = new LinkedHashMap<>();
        for (ZLinkMeshPeerSnapshot peer : status.peers()) {
            if (peer.state() != ZLinkPeerState.READY) continue;
            String routingId = peer.nodeRid().toString();
            observed.put(routingId, true);
        }
        return observed;
    }
}
