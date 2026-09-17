package systems.zlink.framework.testkit;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import org.junit.jupiter.api.Test;

final class JvmE2eRunnerIsolationContractTest {
    private static final List<String> EARLY_CLEANUP_SAMPLE_RUNNERS = List.of(
        "java/GameQuest/run_sample.sh",
        "java/ShoppingMall/run_sample.sh",
        "java/SupportChat/run_sample.sh",
        "kotlin/GameQuest/run_sample.sh");

    @Test
    void cleanupTrapsPrecedeOwnedResources() throws IOException {
        assertCleanupTrapPrecedesResources(
            javaRoot().resolve("samples"),
            EARLY_CLEANUP_SAMPLE_RUNNERS,
            "trap on_exit EXIT");
    }

    @Test
    void javaGameQuestPollsMissionEvidenceAcrossAllEligibleOwners() throws IOException {
        Path path = javaRoot().resolve("samples/java/GameQuest/run_sample.sh");
        String runner = Files.readString(path)
            .replace("\\\n", " ")
            .replaceAll("\\s+", " ");
        for (String evidence : List.of(
                "gamequest-mission reconciled player=player-alice quest=first-hunt",
                "gamequest-mission replayed player=player-alice generation=")) {
            String collectiveWait = "wait_log_total_count \"" + evidence + "\" 1 "
                + "\"$LOG_DIR/mission-a.log\" \"$LOG_DIR/mission-b.log\"";
            assertTrue(runner.contains(collectiveWait),
                path + " must poll both eligible mission owners together for " + evidence);
            assertFalse(runner.contains(
                    "wait_log_count \"$LOG_DIR/mission-a.log\" \"" + evidence + "\""),
                path + " must not wait for mission-a before checking mission-b for " + evidence);
        }
    }

    private static void assertCleanupTrapPrecedesResources(
            Path root,
            List<String> runners,
            String trapMarker) throws IOException {
        for (String relative : runners) {
            Path path = root.resolve(relative);
            String runner = Files.readString(path);
            int trapOffset = runner.indexOf(trapMarker);
            assertTrue(trapOffset >= 0, path + " must install " + trapMarker);
            for (String resourceMarker : List.of(
                    "mkdir -p", "mktemp", "zlink_sample_reserve_ports",
                    "zlink_sample_reserve_endpoints", "zlink_redis_start_scoped",
                    "docker create", "docker start")) {
                int resourceOffset = runner.indexOf(resourceMarker);
                assertTrue(resourceOffset < 0 || trapOffset < resourceOffset,
                    path + " must install its EXIT trap before " + resourceMarker);
            }
        }
    }

    private static Path javaRoot() {
        return Path.of(System.getProperty("user.dir")).getParent();
    }
}
