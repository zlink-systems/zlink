package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.Env;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class AtdE3ShutdownRecoveryScenario {
    private AtdE3ShutdownRecoveryScenario() {
    }

    public static void run(
        ZLinkStreamConnector connector,
        ZLinkStreamConnector recoveryConnector) {
        String requestId = "ATD-E3-" + UUID.randomUUID();
        Path controlDir = Path.of(Env.get("controlDirectory"));
        CompletableFuture<Contracts.ScenarioRes> pending = connector
            .request(new Contracts.ShutdownAwaitReq(requestId, 5000))
            .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
            .metadata(Contracts.SPOT_RID_METADATA, "room-a")
            .timeout(Duration.ofSeconds(20))
            .submit(Contracts.ScenarioRes.class)
            .toCompletableFuture();

        ClientStreamSupport.waitForEvidence(connector, requestId, "shutdown-await-released");
        touch(controlDir.resolve("atd-e3-ready-to-stop-play"));
        expectPublicFailure(pending);
        waitForFile(controlDir.resolve("atd-e3-play-restarted"));

        Contracts.AwaitShutdownRecoveryRes recovery = requestRecovery(recoveryConnector, requestId);
        ScenarioAssert.that(
            "await.e3-shutdown-recovery".equals(recovery.operation()),
            "ATD-E3 recovery operation mismatch");
        ScenarioAssert.that("room-a".equals(recovery.spotRid()), "ATD-E3 recovery spot mismatch");
        ScenarioAssert.that(
            recovery.evidence().stream().anyMatch(entry ->
                entry.startsWith("probe-completed|") && entry.contains("marker=shutdown-recovery-probe")),
            "ATD-E3 recovery probe marker missing: " + recovery.evidence());
        System.out.println("scenario ATD-E3 passed");
    }

    private static Contracts.AwaitShutdownRecoveryRes requestRecovery(
        ZLinkStreamConnector recovery,
        String requestId) {
        return ClientStreamSupport.await(
            recovery.request(new Contracts.AwaitShutdownRecoveryReq(requestId, "room-a"))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .timeout(Duration.ofSeconds(95)),
            Contracts.AwaitShutdownRecoveryRes.class);
    }

    private static void expectPublicFailure(CompletableFuture<Contracts.ScenarioRes> pending) {
        try {
            pending.get(20, TimeUnit.SECONDS);
        } catch (Exception expected) {
            return;
        }
        throw new IllegalStateException("ATD-E3 pending await request unexpectedly succeeded");
    }

    private static void touch(Path path) {
        try {
            Files.createDirectories(path.getParent());
            Files.writeString(path, "ready\n");
        } catch (Exception error) {
            throw new IllegalStateException("failed to write ATD-E3 control file " + path, error);
        }
    }

    private static void waitForFile(Path path) {
        long deadline = System.nanoTime() + Duration.ofSeconds(70).toNanos();
        while (System.nanoTime() < deadline) {
            if (Files.exists(path)) {
                return;
            }
            ClientStreamSupport.sleep(100);
        }
        throw new IllegalStateException("timed out waiting for ATD-E3 control file " + path);
    }
}
