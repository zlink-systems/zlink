package systems.zlink.samples.gamequest.server.gameapi;

import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import org.springframework.boot.ApplicationArguments;
import org.springframework.boot.ApplicationRunner;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.samples.gamequest.server.configuration.SampleNames;

final class GameQuestReadinessReporter implements ApplicationRunner, AutoCloseable {
    private final String nodeId;
    private final ZLinkRouteMeshRuntime meshes;
    private final ScheduledExecutorService reporter = Executors.newSingleThreadScheduledExecutor(runnable -> {
        Thread thread = new Thread(runnable, "gamequest-readiness");
        thread.setDaemon(true);
        return thread;
    });

    GameQuestReadinessReporter(String nodeId, ZLinkRouteMeshRuntime meshes) {
        this.nodeId = nodeId;
        this.meshes = meshes;
    }

    @Override
    public void run(ApplicationArguments arguments) {
        reporter.scheduleWithFixedDelay(this::report, 0, 100, TimeUnit.MILLISECONDS);
    }

    private void report() {
        try {
            if (meshes.snapshot(SampleNames.PlayerQuestSpotDiscovery).readyPeerCount() == 0) {
                return;
            }
            System.out.printf("gamequest-ready kind=spot-route node=%s mesh=%s%n",
                nodeId, SampleNames.PlayerQuestSpotDiscovery);
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
