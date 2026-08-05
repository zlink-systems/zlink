package systems.zlink.framework;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.Collectors;
import java.util.stream.Stream;
import org.junit.jupiter.api.Test;

final class JavaDocumentationRegressionTest {
    private static final Pattern SNAPSHOT = Pattern.compile("^([0-9a-f]{64}) (\\S+\\.ko\\.md)$");
    private static final Pattern SCENARIO_HEADING = Pattern.compile(
        "^#{1,6}\\s+([A-Z]{2,4}-(?:E2E-)?[A-Z0-9]+(?:-[A-Z0-9]+)*)\\b");
    private static final Pattern SCENARIO_ID = Pattern.compile(
        "\\b([A-Z]{2,4}-(?:E2E-)?[A-Z0-9]+(?:-[A-Z0-9]+)*)\\b");

    @Test
    void canonicalCommonSpecOwnsLiveJavaContracts() {
        Path root = repositoryRoot();
        Path commonSpec = root.resolve("framework/doc/framework/common/spec");
        Path deletedSpec = root.resolve("framework/doc/framework/spec");

        assertFalse(Files.exists(deletedSpec));
        assertTrue(Files.isRegularFile(commonSpec.resolve("13-mesh-node.ko.md")));
        assertTrue(Files.isRegularFile(
            commonSpec.resolve("server/languages/java/02-handler-interfaces.ko.md")));
        assertTrue(Files.isDirectory(
            commonSpec.resolve("server/languages/java/interfaces")));
    }

    @Test
    void everyCommonScenarioIdHasJavaSourceAndRunnerInventory() throws Exception {
        Path root = repositoryRoot();
        Path commonE2e = root.resolve("framework/doc/framework/common/e2e");
        Path javaE2e = root.resolve("framework/languages/java/e2e");

        Set<String> expected = commonScenarioIds(commonE2e);
        Set<String> sourceIds = new HashSet<>();
        try (Stream<Path> files = Files.walk(javaE2e)) {
            files.filter(Files::isRegularFile)
                .filter(path -> {
                    String name = path.getFileName().toString();
                    return name.endsWith(".java") || name.endsWith(".sh");
                })
                .forEach(path -> collectScenarioIds(path, sourceIds));
        }
        Set<String> missing = expected.stream()
            .filter(id -> !sourceIds.contains(id))
            .collect(Collectors.toCollection(java.util.TreeSet::new));

        Map<Integer, String> suites = canonicalSuites();
        Set<String> missingSuites = suites.entrySet().stream()
            .filter(entry -> !Files.isDirectory(javaE2e.resolve(entry.getValue()))
                || !Files.isRegularFile(javaE2e.resolve(entry.getValue()).resolve("run_e2e.sh")))
            .map(entry -> "Config " + entry.getKey() + "=" + entry.getValue())
            .collect(Collectors.toCollection(java.util.TreeSet::new));

        String allRunner = Files.readString(javaE2e.resolve("run_e2e_all.sh"));
        assertTrue(allRunner.contains("validate_selected_suites"));
        assertTrue(allRunner.contains("aggregate_incomplete"));
        Set<String> runnerMissing = suites.values().stream()
            .filter(suite -> !allRunner.contains(suite))
            .collect(Collectors.toCollection(java.util.TreeSet::new));
        assertTrue(missing.isEmpty() && missingSuites.isEmpty() && runnerMissing.isEmpty(),
            "Java E2E inventory mismatch: expected=" + expected.size()
                + ", sourceIds=" + sourceIds.size()
                + ", missingIds=" + missing
                + ", missingSuites=" + missingSuites
                + ", aggregateRunnerMissing=" + runnerMissing);
    }

    @Test
    void everyCommonScenarioIdHasKotlinSourceAndRunnerInventory() throws Exception {
        Path root = repositoryRoot();
        Path commonE2e = root.resolve("framework/doc/framework/common/e2e");
        Path kotlinE2e = root.resolve("framework/languages/java/e2e-kotlin");

        Set<String> expected = commonScenarioIds(commonE2e);
        Set<String> sourceIds = new HashSet<>();
        try (Stream<Path> files = Files.walk(kotlinE2e)) {
            files.filter(Files::isRegularFile)
                .filter(path -> {
                    String name = path.getFileName().toString();
                    return name.endsWith(".kt") || name.endsWith(".java") || name.endsWith(".sh");
                })
                .forEach(path -> collectScenarioIds(path, sourceIds));
        }
        Set<String> missing = expected.stream()
            .filter(id -> !sourceIds.contains(id))
            .collect(Collectors.toCollection(java.util.TreeSet::new));

        Map<Integer, String> suites = canonicalSuites();
        Set<String> missingSuites = suites.entrySet().stream()
            .filter(entry -> !Files.isDirectory(kotlinE2e.resolve(entry.getValue()))
                || !Files.isRegularFile(kotlinE2e.resolve(entry.getValue()).resolve("run_e2e.sh")))
            .map(entry -> "Config " + entry.getKey() + "=" + entry.getValue())
            .collect(Collectors.toCollection(java.util.TreeSet::new));

        String allRunner = Files.readString(kotlinE2e.resolve("run_e2e_all.sh"));
        assertTrue(allRunner.contains("validate_selected_suites"));
        assertTrue(allRunner.contains("aggregate_incomplete"));
        Set<String> runnerMissing = suites.values().stream()
            .filter(suite -> !allRunner.contains(suite))
            .collect(Collectors.toCollection(java.util.TreeSet::new));
        assertTrue(missing.isEmpty() && missingSuites.isEmpty() && runnerMissing.isEmpty(),
            "Kotlin E2E inventory mismatch: expected=" + expected.size()
                + ", sourceIds=" + sourceIds.size()
                + ", missingIds=" + missing
                + ", missingSuites=" + missingSuites
                + ", aggregateRunnerMissing=" + runnerMissing);
    }

    private static Set<String> commonScenarioIds(Path commonE2e) throws IOException {
        Set<String> expected = new HashSet<>();
        try (Stream<Path> files = Files.list(commonE2e)) {
            for (Path file : files
                .filter(path -> path.getFileName().toString().matches("config-[0-9]+-.+\\.ko\\.md"))
                .toList()) {
                for (String line : Files.readAllLines(file, StandardCharsets.UTF_8)) {
                    Matcher matcher = SCENARIO_HEADING.matcher(line);
                    if (matcher.find()) expected.add(matcher.group(1));
                }
            }
        }
        return expected;
    }

    private static Map<Integer, String> canonicalSuites() {
        return Map.ofEntries(
            Map.entry(1, "RegistryMessaging"),
            Map.entry(2, "SpotService"),
            Map.entry(3, "PubSub"),
            Map.entry(4, "RegistrationCodec"),
            Map.entry(5, "ResilienceLifecycle"),
            Map.entry(6, "StoreFailure"),
            Map.entry(7, "RuntimeMonitoring"),
            Map.entry(8, "AutomaticTurnDispatch"),
            Map.entry(9, "ToActorMessaging"),
            Map.entry(10, "SpotActorTransfer"),
            Map.entry(11, "ObservabilityOps"),
            Map.entry(12, "ChannelEgressRouting"),
            Map.entry(13, "SubmitAdmission"),
            Map.entry(14, "InstanceSpot"));
    }

    private static void collectScenarioIds(Path path, Set<String> target) {
        try {
            for (String line : Files.readAllLines(path, StandardCharsets.UTF_8)) {
                Matcher matcher = SCENARIO_ID.matcher(line);
                while (matcher.find()) target.add(matcher.group(1));
            }
        } catch (IOException error) {
            throw new java.io.UncheckedIOException(error);
        }
    }

    private static Map<String, String> parseSnapshot(String ledger) {
        Map<String, String> result = new HashMap<>();
        for (String line : ledger.lines().toList()) {
            Matcher matcher = SNAPSHOT.matcher(line);
            if (matcher.matches()) result.put(matcher.group(2), matcher.group(1));
        }
        return Map.copyOf(result);
    }

    private static Path repositoryRoot() {
        Path current = Path.of("").toAbsolutePath();
        while (current != null) {
            if (Files.isRegularFile(current.resolve("framework/languages/java/settings.gradle.kts"))
                && Files.isDirectory(current.resolve("framework/doc/framework/common/spec"))) {
                return current;
            }
            current = current.getParent();
        }
        throw new IllegalStateException("repository root not found");
    }

    private static String readUnchecked(Path path) {
        try {
            return Files.readString(path, StandardCharsets.UTF_8);
        } catch (IOException error) {
            throw new java.io.UncheckedIOException(error);
        }
    }
}
