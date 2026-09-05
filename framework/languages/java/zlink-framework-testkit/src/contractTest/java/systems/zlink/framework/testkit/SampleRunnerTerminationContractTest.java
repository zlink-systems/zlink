package systems.zlink.framework.testkit;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

final class SampleRunnerTerminationContractTest {
    @TempDir
    Path temporaryRoot;

    @Test
    void teardownFailureEscapesExitTrapAndAggregateConditionalWithoutPidArray()
        throws Exception {
        Path logs = writeRoleLog("teardown",
            "ZLINK_FRAMEWORK_READY\n"
                + "ZLINK_FRAMEWORK_TERMINATION outcome=FORCE_STOPPED reason=TEARDOWN_FAILED\n");
        Path sample = temporaryRoot.resolve("sample.sh");
        Files.writeString(sample, """
            #!/usr/bin/env bash
            source "$1"
            log_dir="$2"
            ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="role.log"
            trap 'cleanup; exit 0' EXIT
            exit 0
            """);
        Process process = new ProcessBuilder("bash", "-c", """
            if bash "$1" "$2" "$3"; then
              exit 0
            else
              exit 1
            fi
            """, "aggregate", sample.toString(),
            samplesRoot().resolve("runner-common.sh").toString(), logs.toString())
            .redirectErrorStream(true).start();
        assertTrue(process.waitFor(10, TimeUnit.SECONDS));
        String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        assertNotEquals(0, process.exitValue(), output);
        assertTrue(output.contains("Framework lifecycle evidence is incomplete"), output);
    }

    @Test
    void restartChecksCurrentProcessAndExcludesOnlyTheIntentionalCrash() throws Exception {
        Path logs = writeRoleLog("restarted",
            "ZLINK_FRAMEWORK_READY\n"
                + "ZLINK_FRAMEWORK_READY\n"
                + "ZLINK_FRAMEWORK_TERMINATION outcome=FORCE_STOPPED reason=TEARDOWN_FAILED\n");
        Process process = new ProcessBuilder("bash", "-c", """
            source "$1"
            ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="role.log"
            declare -A ZLINK_SAMPLE_FRAMEWORK_ROLE_LOG_OFFSETS=([role.log]=2)
            zlink_sample_verify_framework_termination "$2"
            """, "restart", samplesRoot().resolve("runner-common.sh").toString(), logs.toString())
            .redirectErrorStream(true).start();
        assertTrue(process.waitFor(10, TimeUnit.SECONDS));
        String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        assertNotEquals(0, process.exitValue(), output);
        assertTrue(output.contains("READY=1 TERMINATION=1 STOPPED_NONE=0 FORCE_STOPPED=1"), output);
    }

    @Test
    void cleanupAcceptsOnlyGracefulFrameworkTerminationForBothLogDirectoryNames()
        throws Exception {
        for (String variableName : List.of("log_dir", "LOG_DIR")) {
            Path gracefulLogs = writeRoleLog(
                variableName + "-graceful",
                "ZLINK_FRAMEWORK_READY\n"
                    + "ZLINK_FRAMEWORK_TERMINATION outcome=STOPPED reason=NONE\n");
            Path gracefulPreserved = Files.createDirectories(
                temporaryRoot.resolve(variableName + "-graceful-preserved"));

            ShellResult graceful = runCleanup(variableName, gracefulLogs, gracefulPreserved);

            assertEquals(0, graceful.exitCode(), graceful.output());
            assertTrue(isEmpty(gracefulPreserved),
                "a graceful role must not create failure evidence for " + variableName);

            Path forcedLogs = writeRoleLog(
                variableName + "-forced",
                "ZLINK_FRAMEWORK_READY\n"
                    + "ZLINK_FRAMEWORK_TERMINATION outcome=FORCE_STOPPED "
                    + "reason=DEADLINE_EXCEEDED\n");
            Path forcedPreserved = Files.createDirectories(
                temporaryRoot.resolve(variableName + "-forced-preserved"));

            ShellResult forced = runCleanup(variableName, forcedLogs, forcedPreserved);

            assertNotEquals(0, forced.exitCode(), forced.output());
            assertTrue(containsPreservedRoleLog(forcedPreserved),
                "a forced role must preserve its log for " + variableName);
        }
    }

    private Path writeRoleLog(String directoryName, String contents) throws IOException {
        Path logDirectory = Files.createDirectories(temporaryRoot.resolve(directoryName));
        Files.writeString(logDirectory.resolve("role.log"), contents);
        return logDirectory;
    }

    private ShellResult runCleanup(
        String variableName,
        Path logDirectory,
        Path preservedRoot) throws Exception {
        Path commonRunner = samplesRoot().resolve("runner-common.sh");
        String script = """
            source "$1"
            %s="$2"
            ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="role.log"
            ZLINK_SAMPLE_FAILURE_LOG_ROOT="$3"
            ZLINK_SAMPLE_FAILURE_LOG_PREFIX="termination-contract"
            pids=()
            true
            cleanup
            """.formatted(variableName);
        Process process = new ProcessBuilder(
            "bash",
            "-c",
            script,
            "sample-runner-termination-contract",
            commonRunner.toString(),
            logDirectory.toString(),
            preservedRoot.toString())
            .redirectErrorStream(true)
            .start();
        if (!process.waitFor(10, TimeUnit.SECONDS)) {
            process.destroyForcibly();
            throw new AssertionError("sample cleanup contract timed out for " + variableName);
        }
        String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        return new ShellResult(process.exitValue(), output);
    }

    private static boolean isEmpty(Path directory) throws IOException {
        try (var children = Files.list(directory)) {
            return children.findAny().isEmpty();
        }
    }

    private static boolean containsPreservedRoleLog(Path directory) throws IOException {
        try (var paths = Files.walk(directory)) {
            return paths.anyMatch(path -> path.getFileName().toString().equals("role.log"));
        }
    }

    private static Path samplesRoot() {
        Path current = Path.of("").toAbsolutePath();
        for (Path candidate = current; candidate != null; candidate = candidate.getParent()) {
            Path direct = candidate.resolve("samples");
            if (Files.isRegularFile(direct.resolve("runner-common.sh"))) {
                return direct;
            }
            Path repository = candidate.resolve("framework/languages/java/samples");
            if (Files.isRegularFile(repository.resolve("runner-common.sh"))) {
                return repository;
            }
        }
        throw new IllegalStateException("cannot locate framework/languages/java/samples");
    }

    private record ShellResult(int exitCode, String output) {
    }
}
