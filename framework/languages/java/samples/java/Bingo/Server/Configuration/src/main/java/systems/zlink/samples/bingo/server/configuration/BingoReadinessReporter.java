package systems.zlink.samples.bingo.server.configuration;

import java.util.List;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.boot.ApplicationArguments;
import org.springframework.boot.ApplicationRunner;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;

public final class BingoReadinessReporter implements ApplicationRunner, AutoCloseable {
    private static final Logger logger = LoggerFactory.getLogger(BingoReadinessReporter.class);

    private final ScheduledExecutorService reporter;
    private final List<ReadinessCheck> checks;

    private BingoReadinessReporter(List<ReadinessCheck> checks) {
        this.checks = checks;
        this.reporter = Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, "bingo-readiness");
            thread.setDaemon(true);
            return thread;
        });
    }

    public static BingoReadinessReporter api(
        SampleTopology topology,
        ZLinkRouteMeshRuntime meshes) {
        String node = "api-" + topology.apiNode();
        return new BingoReadinessReporter(List.of(
            new ReadinessCheck(
                "bingo-ready kind=mesh-route node=" + node + " mesh=matchmaking",
                () -> meshes.snapshot(SampleNames.MatchmakingMesh).readyPeerCount() > 0),
            new ReadinessCheck(
                "bingo-ready kind=mesh-route node=" + node + " mesh=room",
                () -> meshes.snapshot(SampleNames.Mesh).readyPeerCount() > 0)));
    }

    public static BingoReadinessReporter play(
        SampleTopology topology,
        ZLinkRouteMeshRuntime meshes) {
        String node = "play-" + topology.playNode();
        String peer = "play-" + ("a".equals(topology.playNode()) ? "b" : "a");
        return new BingoReadinessReporter(List.of(
            new ReadinessCheck(
                "bingo-ready kind=peer-route node=" + node + " peer=" + peer,
                () -> meshes.snapshot(SampleNames.Mesh).readyPeerCount() >= 5)));
    }

    public static BingoReadinessReporter session(
        SampleTopology topology,
        ZLinkRouteMeshRuntime meshes) {
        String node = "session-" + topology.sessionNode();
        return new BingoReadinessReporter(List.of(
            new ReadinessCheck(
                "bingo-ready kind=mesh-route node=" + node + " mesh=room",
                () -> meshes.snapshot(SampleNames.Mesh).readyPeerCount() > 0)));
    }

    @Override
    public void run(ApplicationArguments arguments) {
        reporter.scheduleWithFixedDelay(this::report, 0, 100, TimeUnit.MILLISECONDS);
    }

    private void report() {
        for (ReadinessCheck check : checks) {
            check.reportIfReady();
        }
        if (checks.stream().allMatch(ReadinessCheck::reported)) {
            reporter.shutdown();
        }
    }

    @Override
    public void close() {
        reporter.shutdownNow();
    }

    private static final class ReadinessCheck {
        private final String evidence;
        private final BooleanSupplier ready;
        private boolean reported;

        private ReadinessCheck(String evidence, BooleanSupplier ready) {
            this.evidence = evidence;
            this.ready = ready;
        }

        private void reportIfReady() {
            if (reported) {
                return;
            }
            try {
                if (ready.getAsBoolean()) {
                    logger.info(evidence);
                    reported = true;
                }
            } catch (IllegalStateException | ZLinkConfigurationException ignored) {
                // The public runtime view is not available until Framework startup completes.
            }
        }

        private boolean reported() {
            return reported;
        }
    }
}
