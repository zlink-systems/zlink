package systems.zlink.framework.testkit;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.stream.Stream;
import org.junit.jupiter.api.Test;

final class JvmE2eRunnerIsolationContractTest {
    private static final String SHARED_GRADLE_LOCK =
        "/tmp/zlink-framework-java-kotlin-sample-gradle.lock";
    private static final List<String> JAVA_RUNNERS = List.of(
        "AutomaticTurnDispatch/run_e2e.sh",
        "ChannelEgressRouting/run_e2e.sh",
        "InstanceSpot/run_e2e.sh",
        "ObservabilityOps/run_a5_e2e.sh",
        "ObservabilityOps/run_c_e2e.sh",
        "ObservabilityOps/run_e2e.sh",
        "PubSub/run_e2e.sh",
        "RegistrationCodec/run_e2e.sh",
        "RegistryMessaging/run_e2e.sh",
        "ResilienceLifecycle/run_e2e.sh",
        "RuntimeMonitoring/run_e2e.sh",
        "SpotActorTransfer/run_e2e.sh",
        "SpotService/run_e2e.sh",
        "StoreFailure/run_e2e.sh",
        "SubmitAdmission/run_e2e.sh",
        "ToActorMessaging/run_e2e.sh");
    private static final List<String> KOTLIN_RUNNERS = List.of(
        "AutomaticTurnDispatch/run_e2e.sh",
        "ChannelEgressRouting/run_e2e.sh",
        "DiscoveryRegistryHa/run_e2e.sh",
        "InstanceSpot/run_e2e.sh",
        "ObservabilityOps/run_a5_e2e.sh",
        "ObservabilityOps/run_e2e.sh",
        "PubSub/run_e2e.sh",
        "RegistrationCodec/run_e2e.sh",
        "RegistryMessaging/run_e2e.sh",
        "ResilienceLifecycle/run_e2e.sh",
        "RuntimeMonitoring/run_e2e.sh",
        "SpotActorTransfer/run_e2e.sh",
        "SpotService/run_e2e.sh",
        "StoreFailure/run_e2e.sh",
        "SubmitAdmission/run_e2e.sh",
        "ToActorMessaging/run_e2e.sh");
    private static final List<String> EARLY_CLEANUP_JAVA_E2E_RUNNERS = List.of(
        "AutomaticTurnDispatch/run_e2e.sh",
        "ObservabilityOps/run_e2e.sh",
        "ObservabilityOps/run_c_e2e.sh",
        "SpotService/run_e2e.sh",
        "ToActorMessaging/run_e2e.sh");
    private static final List<String> EARLY_CLEANUP_KOTLIN_E2E_RUNNERS = List.of(
        "ObservabilityOps/run_e2e.sh",
        "ToActorMessaging/run_e2e.sh");
    private static final List<String> EARLY_CLEANUP_SAMPLE_RUNNERS = List.of(
        "java/GameQuest/run_sample.sh",
        "java/ShoppingMall/run_sample.sh",
        "java/SupportChat/run_sample.sh",
        "kotlin/GameQuest/run_sample.sh");

    @Test
    void commonHelperOwnsLanguagePoolsAndRunLocks() throws IOException {
        String helper = Files.readString(javaRoot().resolve("e2e-runner-common.sh"));
        for (String needle : List.of(
                "ZLINK_E2E_REDIS_PORT_MIN=34000",
                "ZLINK_E2E_REDIS_PORT_MAX=34099",
                "ZLINK_E2E_APP_PORT_MIN=34100",
                "ZLINK_E2E_APP_PORT_MAX=35999",
                "ZLINK_E2E_REDIS_PORT_MIN=36000",
                "ZLINK_E2E_REDIS_PORT_MAX=36099",
                "ZLINK_E2E_APP_PORT_MIN=36100",
                "ZLINK_E2E_APP_PORT_MAX=37999",
                "/tmp/zlink-framework-java-e2e-run.lock",
                "/tmp/zlink-framework-kotlin-e2e-run.lock",
                "flock --exclusive --close",
                "bash \"${runner}\" \"$@\"",
                "ZLINK_E2E_RUNNER_LANGUAGE_OVERRIDE",
                "sock.bind((\"127.0.0.1\", port))",
                "zlink_e2e_assign_unique_ports")) {
            assertTrue(helper.contains(needle), "JVM E2E helper must contain " + needle);
        }
        assertTrue(helper.contains(SHARED_GRADLE_LOCK),
            "JVM E2E builds must use the sample Gradle lock");
        assertTrue(helper.contains("flock --exclusive --close \"${lock_path}\" \"$@\""),
            "JVM E2E builds must not leak the build lock into Gradle descendants");
        assertTrue(Files.readString(javaRoot().resolve("samples/runner-common.sh"))
                .contains(SHARED_GRADLE_LOCK),
            "samples and E2Es must serialize Gradle with the same lock file");
    }

    @Test
    void redisHelperUsesExplicitVerifiedHostPorts() throws IOException {
        String helper = Files.readString(javaRoot().resolve("e2e-redis-common.sh"));
        for (String needle : List.of(
                "zlink_e2e_reserve_ports_in_range 1",
                "for ((offset=0; offset<range_size; offset++))",
                "127.0.0.1:${host_port}:6379",
                "docker create",
                "docker start",
                "published_port",
                "\"${published_port}\" != \"${host_port}\"",
                "zlink_redis_remove_attempt \"${container_id}\" \"${name}\"",
                "docker inspect --type container",
                "address already in use|port is already allocated|failed to bind host port",
                "zlink_redis_remove_by_id",
                "[[ \"${container_id}\" =~ ^[0-9a-f]{12,64}$ ]] || return 1",
                "timeout -k 2s \"${docker_timeout_seconds}s\" docker rm -fv")) {
            assertTrue(helper.contains(needle), "JVM E2E Redis helper must contain " + needle);
        }
        assertFalse(helper.contains("127.0.0.1::6379"),
            "JVM E2E Redis must not request a dynamic Docker host port");
    }

    @Test
    void shortClientDockerControlUsesABoundedProcessTreeCleanup() throws IOException {
        Path contextPath = e2eRoot().resolve(
            "RuntimeMonitoring/Client/src/main/java/systems/zlink/e2e/"
                + "runtimemonitoring/client/Support/MonitoringScenarioContext.java");
        String context = Files.readString(contextPath);
        for (String needle : List.of(
                "process.waitFor(10, TimeUnit.SECONDS)",
                "process.descendants().forEach(ProcessHandle::destroyForcibly)",
                "process.destroyForcibly()",
                "process.waitFor(2, TimeUnit.SECONDS)",
                "timed out after 10 seconds")) {
            assertTrue(context.contains(needle),
                contextPath + " must contain " + needle);
        }
        assertFalse(context.contains("process.waitFor() == 0"),
            contextPath + " must not wait indefinitely for Docker control");
    }

    @Test
    void everyStandaloneRunnerUsesItsLanguageIsolation() throws IOException {
        assertRunnerIsolation(e2eRoot(), JAVA_RUNNERS, "java");
        assertRunnerIsolation(kotlinE2eRoot(), KOTLIN_RUNNERS, "kotlin");

        Path kotlinSpotTransferPath =
            kotlinE2eRoot().resolve("SpotActorTransfer/run_e2e.sh");
        String kotlinSpotTransfer = Files.readString(kotlinSpotTransferPath);
        int overrideOffset = kotlinSpotTransfer.indexOf(
            "export ZLINK_E2E_RUNNER_LANGUAGE_OVERRIDE=kotlin");
        int initializeOffset = kotlinSpotTransfer.indexOf(
            "zlink_e2e_initialize kotlin \"$0\" \"$@\"");
        assertTrue(overrideOffset >= 0,
            "the Kotlin wrapper must keep Kotlin isolation when it reuses the Java runner");
        assertTrue(initializeOffset >= 0 && overrideOffset < initializeOffset,
            kotlinSpotTransferPath + " must set its Kotlin override before initialization");
    }

    @Test
    void runnerInventoryMatchesDiscoveredShellRunners() throws IOException {
        assertEquals(JAVA_RUNNERS, discoverRunners(e2eRoot()),
            "the Java runner inventory must exactly match run*_e2e.sh discovery");
        assertEquals(KOTLIN_RUNNERS, discoverRunners(kotlinE2eRoot()),
            "the Kotlin runner inventory must exactly match run*_e2e.sh discovery");
    }

    @Test
    void cleanupTrapsPrecedeOwnedResources() throws IOException {
        assertCleanupTrapPrecedesResources(
            e2eRoot(), EARLY_CLEANUP_JAVA_E2E_RUNNERS, "trap cleanup EXIT");
        assertCleanupTrapPrecedesResources(
            kotlinE2eRoot(), EARLY_CLEANUP_KOTLIN_E2E_RUNNERS, "trap cleanup EXIT");
        assertCleanupTrapPrecedesResources(
            javaRoot().resolve("samples"),
            EARLY_CLEANUP_SAMPLE_RUNNERS,
            "trap on_exit EXIT");
    }

    @Test
    void aggregateRunnersLeaveWholeRunLocksToChildren() throws IOException {
        for (Path aggregate : List.of(
                e2eRoot().resolve("run_e2e_all.sh"),
                kotlinE2eRoot().resolve("run_e2e_all.sh"))) {
            String runner = Files.readString(aggregate);
            assertFalse(runner.contains("zlink_e2e_initialize")
                    || runner.contains("e2e-runner-common.sh"),
                aggregate + " must not hold a whole-run lock while invoking children");
            assertTrue(runner.contains("[[ ! -f \"$runner\" ]]"),
                aggregate + " must validate runner files without requiring executable mode");
            assertFalse(runner.contains("[[ ! -x \"$runner\" ]]"),
                aggregate + " must not reject a 0644 runner");
            assertTrue(runner.contains("bash ./run_e2e.sh"),
                aggregate + " must invoke child runners explicitly with Bash");
        }

        Path javaAggregatePath = e2eRoot().resolve("run_e2e_all.sh");
        String javaAggregate = Files.readString(javaAggregatePath);
        int childLaunchOffset = javaAggregate.indexOf(
            "bash ./run_e2e.sh \"${selector}\" --start-order \"${start_order}\"");
        int activePidOffset = javaAggregate.indexOf(
            "active_scenario_pid=\"$!\"", childLaunchOffset);
        int waitOffset = javaAggregate.indexOf(
            "wait \"${active_scenario_pid}\"", activePidOffset);
        assertTrue(childLaunchOffset >= 0
                && activePidOffset > childLaunchOffset
                && waitOffset > activePidOffset,
            javaAggregatePath + " must wait for each launched child before continuing");
        assertForegroundAggregateCalls(javaAggregatePath, javaAggregate, 2);

        Path kotlinAggregatePath = kotlinE2eRoot().resolve("run_e2e_all.sh");
        assertForegroundAggregateCalls(
            kotlinAggregatePath, Files.readString(kotlinAggregatePath), 1);
    }

    @Test
    void recursiveRunnerCallsDoNotRequireExecutableMode() throws IOException {
        assertContains(e2eRoot().resolve("SpotActorTransfer/run_e2e.sh"),
            "bash \"${BASH_SOURCE[0]}\" \"${scenario}\"");
        assertContains(e2eRoot().resolve("RuntimeMonitoring/run_e2e.sh"),
            "bash \"${BASH_SOURCE[0]}\" \"${scenario}\"");
        assertContains(e2eRoot().resolve("ObservabilityOps/run_e2e.sh"),
            "bash \"${BASH_SOURCE[0]}\" \"${selector}\"");
        assertContains(e2eRoot().resolve("SpotService/run_e2e.sh"),
            "timeout 900s bash \"${SCRIPT_PATH}\"");
        assertContains(kotlinE2eRoot().resolve("SpotService/run_e2e.sh"),
            "ZLINK_SPOT_SERVICE_RETRY_CHILD=1 bash \"${SCRIPT_PATH}\"");
        assertContains(kotlinE2eRoot().resolve("SpotService/run_e2e.sh"),
            "ZLINK_SPOT_SERVICE_ALL_CHILD=1 bash \"${SCRIPT_PATH}\"");
        assertContains(e2eRoot().resolve("PubSub/run_e2e.sh"),
            "ZLINK_JAVA_E2E_SKIP_BUILD=true bash \"$(pwd)/run_e2e.sh\"");
        assertContains(kotlinE2eRoot().resolve("PubSub/run_e2e.sh"),
            "ZLINK_KOTLIN_E2E_SKIP_BUILD=true bash \"${ROOT_DIR}/run_e2e.sh\"");
        assertContains(kotlinE2eRoot().resolve("RegistryMessaging/run_e2e.sh"),
            "bash \"${ROOT_DIR}/run_e2e.sh\"");
        assertContains(kotlinE2eRoot().resolve("SpotActorTransfer/run_e2e.sh"),
            "bash \"${JAVA_SCENARIO_DIR}/run_e2e.sh\"");

        Path storeFailure = kotlinE2eRoot().resolve("StoreFailure/run_e2e.sh");
        String storeFailureRunner = Files.readString(storeFailure);
        assertTrue(storeFailureRunner.contains("[[ ! -f \"${LEGACY_RUNNER}\" ]]"),
            storeFailure + " must validate the delegated runner as a file");
        assertTrue(storeFailureRunner.contains("exec bash \"${LEGACY_RUNNER}\""),
            storeFailure + " must invoke the delegated runner explicitly with Bash");
        assertFalse(storeFailureRunner.contains("[[ ! -x \"${LEGACY_RUNNER}\" ]]"),
            storeFailure + " must not reject a 0644 delegated runner");
    }

    private static void assertRunnerIsolation(
            Path root,
            List<String> runners,
            String language) throws IOException {
        for (String relative : runners) {
            Path path = root.resolve(relative);
            String runner = Files.readString(path);
            String initialize = "zlink_e2e_initialize " + language + " \"$0\" \"$@\"";
            int lockOffset = runner.indexOf(initialize);
            assertTrue(lockOffset >= 0,
                path + " must acquire the " + language + " whole-run lock");
            for (String resourceMarker : List.of(
                    "RUN_ID=", "run_id=", "mktemp", "zlink_e2e_reserve_ports",
                    "zlink_redis_start_scoped", "docker create", "docker start")) {
                int resourceOffset = runner.indexOf(resourceMarker);
                assertTrue(resourceOffset < 0 || lockOffset < resourceOffset,
                    path + " must acquire the whole-run lock before " + resourceMarker);
            }
            assertFalse(runner.contains("127.0.0.1::6379")
                    || runner.contains("sock.bind((\"127.0.0.1\", 0))")
                    || runner.contains("s.bind(('127.0.0.1', 0))")
                    || runner.contains("s.bind(('127.0.0.1',0))"),
                path + " must use the shared Redis and application port allocators");
            assertDockerCommandsBounded(path, runner);
            assertGradleCommandsLocked(path, runner);
        }
    }

    private static void assertDockerCommandsBounded(Path path, String runner) {
        assertFalse(runner.contains("docker rm -fv"),
            path + " must delegate exact-ID Redis cleanup to the shared helper");
        for (String line : runner.lines().toList()) {
            String trimmed = line.trim();
            boolean shortCommand = Stream.of(
                    "docker pause ", "docker unpause ", "docker stop ",
                    "docker start ", "docker inspect ", "docker exec ")
                .anyMatch(trimmed::contains);
            if (!shortCommand || trimmed.contains("redis-cli --csv monitor")) {
                continue;
            }
            assertTrue(trimmed.contains("timeout "),
                path + " has an unbounded short Docker control command: " + trimmed);
        }
    }

    private static List<String> discoverRunners(Path root) throws IOException {
        try (Stream<Path> paths = Files.walk(root)) {
            return paths
                .filter(Files::isRegularFile)
                .filter(path -> path.getFileName().toString().matches("run.*_e2e\\.sh"))
                .map(root::relativize)
                .map(path -> path.toString().replace('\\', '/'))
                .sorted()
                .toList();
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
                    "mkdir -p", "mktemp", "zlink_e2e_reserve_ports",
                    "zlink_e2e_reserve_mixed_endpoints", "zlink_sample_reserve_ports",
                    "zlink_sample_reserve_endpoints", "zlink_redis_start_scoped",
                    "docker create", "docker start")) {
                int resourceOffset = runner.indexOf(resourceMarker);
                assertTrue(resourceOffset < 0 || trapOffset < resourceOffset,
                    path + " must install its EXIT trap before " + resourceMarker);
            }
        }
    }

    private static void assertGradleCommandsLocked(Path path, String runner) {
        int offset = 0;
        while ((offset = runner.indexOf("gradlew", offset)) >= 0) {
            int windowStart = offset;
            for (int line = 0; line < 8 && windowStart > 0; line++) {
                int previous = runner.lastIndexOf('\n', windowStart - 1);
                windowStart = previous < 0 ? 0 : previous;
            }
            assertTrue(runner.substring(windowStart, offset)
                    .contains("zlink_e2e_gradle_build_locked"),
                path + " has a Gradle invocation outside the shared build-only lock");
            offset += "gradlew".length();
        }
    }

    private static void assertForegroundAggregateCalls(
            Path path,
            String runner,
            long expectedCount) {
        List<String> calls = runner.lines()
            .map(String::trim)
            .filter(line -> line.startsWith("run_scenario_with_retry "))
            .toList();
        assertEquals(expectedCount, calls.size(),
            path + " must keep the expected sequential aggregate calls");
        assertTrue(calls.stream().noneMatch(line -> line.endsWith("&")),
            path + " must invoke run_scenario_with_retry in the foreground");
    }

    private static void assertContains(Path path, String needle) throws IOException {
        assertTrue(Files.readString(path).contains(needle),
            path + " must contain " + needle);
    }

    private static Path e2eRoot() {
        return javaRoot().resolve("e2e");
    }

    private static Path kotlinE2eRoot() {
        return javaRoot().resolve("e2e-kotlin");
    }

    private static Path javaRoot() {
        return Path.of(System.getProperty("user.dir")).getParent();
    }
}
