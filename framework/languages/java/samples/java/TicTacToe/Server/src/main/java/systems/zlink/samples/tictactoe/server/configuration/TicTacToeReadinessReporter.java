package systems.zlink.samples.tictactoe.server.configuration;

import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.boot.ApplicationArguments;
import org.springframework.boot.ApplicationRunner;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;

public final class TicTacToeReadinessReporter implements ApplicationRunner, AutoCloseable {
    private static final Logger LOGGER = LoggerFactory.getLogger(TicTacToeReadinessReporter.class);
    private final String nodeId;
    private final String peerNodeId;
    private final RoutingId expectedPeerRoutingId;
    private final ZLinkRouteMeshRuntime meshes;
    private final ScheduledExecutorService reporter = Executors.newSingleThreadScheduledExecutor(runnable -> {
        Thread thread = new Thread(runnable, "tictactoe-readiness");
        thread.setDaemon(true);
        return thread;
    });

    private TicTacToeReadinessReporter(
        String nodeId,
        String peerNodeId,
        RoutingId expectedPeerRoutingId,
        ZLinkRouteMeshRuntime meshes) {
        this.nodeId = nodeId;
        this.peerNodeId = peerNodeId;
        this.expectedPeerRoutingId = expectedPeerRoutingId;
        this.meshes = meshes;
    }

    public static TicTacToeReadinessReporter peerRoute(
        String nodeId,
        String peerNodeId,
        ZLinkRouteMeshRuntime meshes) {
        return new TicTacToeReadinessReporter(
            nodeId,
            peerNodeId,
            RoutingId.from("tictactoe-" + peerNodeId),
            meshes);
    }

    public static TicTacToeReadinessReporter spotRoute(
        String nodeId,
        ZLinkRouteMeshRuntime meshes) {
        return new TicTacToeReadinessReporter(nodeId, null, null, meshes);
    }

    @Override
    public void run(ApplicationArguments arguments) {
        reporter.scheduleWithFixedDelay(this::report, 0, 100, TimeUnit.MILLISECONDS);
    }

    private void report() {
        try {
            var snapshot = meshes.snapshot(SampleNames.SpotMesh);
            boolean ready = expectedPeerRoutingId == null
                ? snapshot.readyPeerCount() >= 2
                : snapshot.peers().stream().anyMatch(peer ->
                    peer.nodeRid().equals(expectedPeerRoutingId)
                        && peer.state() == ZLinkPeerState.READY);
            if (!ready) return;
            if (expectedPeerRoutingId == null) {
                LOGGER.info(
                    "tictactoe-ready kind=spot-route node={} mesh={}",
                    nodeId,
                    SampleNames.SpotMesh);
            } else {
                LOGGER.info("tictactoe-ready kind=peer-route node={} peer={}", nodeId, peerNodeId);
            }
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
