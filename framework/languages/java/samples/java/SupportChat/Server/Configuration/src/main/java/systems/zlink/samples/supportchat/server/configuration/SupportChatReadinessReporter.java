package systems.zlink.samples.supportchat.server.configuration;

import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import org.springframework.boot.ApplicationArguments;
import org.springframework.boot.ApplicationRunner;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;

public final class SupportChatReadinessReporter implements ApplicationRunner, AutoCloseable {
    private final String nodeId;
    private final ZLinkRouteMeshRuntime meshes;
    private final ScheduledExecutorService reporter = Executors.newSingleThreadScheduledExecutor(runnable -> {
        Thread thread = new Thread(runnable, "supportchat-readiness");
        thread.setDaemon(true);
        return thread;
    });

    public SupportChatReadinessReporter(String nodeId, ZLinkRouteMeshRuntime meshes) {
        this.nodeId = nodeId;
        this.meshes = meshes;
    }

    @Override
    public void run(ApplicationArguments arguments) {
        reporter.scheduleWithFixedDelay(this::report, 0, 100, TimeUnit.MILLISECONDS);
    }

    private void report() {
        try {
            boolean supportRouteReady = meshes.snapshot(SampleNames.SupportActorMesh).peers().stream()
                .anyMatch(peer -> peer.nodeRid().equals(SampleNames.SupportNodeRoutingId)
                    && peer.state() == ZLinkPeerState.READY);
            if (!supportRouteReady) {
                return;
            }
            System.out.printf("supportchat-ready kind=spot-route node=%s mesh=%s%n",
                nodeId, SampleNames.SupportActorMesh);
            reporter.shutdown();
        } catch (IllegalStateException | ZLinkConfigurationException ignored) {
            // The public runtime view is not available until Framework startup completes.
        }
    }

    @Override
    public void close() {
        reporter.shutdownNow();
    }
}
