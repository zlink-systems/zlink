package systems.zlink.framework;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class JavaDocumentationRegressionTest {
    private static final Pattern SNAPSHOT = Pattern.compile("^([0-9a-f]{64}) (\\S+\\.ko\\.md)$");

    @Test
    void canonicalCommonSpecOwnsLiveJavaContracts() {
        Path root = repositoryRoot();
        Path commonSpec = root.resolve("framework/doc/framework/common/spec");
        Path deletedSpec = root.resolve("framework/doc/framework/spec");

        assertFalse(Files.exists(deletedSpec));
        assertTrue(
                Files.isRegularFile(commonSpec.resolve("server/03-spot-actor/03-mesh-node.ko.md")));
        assertTrue(
                Files.isRegularFile(
                        commonSpec.resolve("server/languages/java/02-handler-interfaces.ko.md")));
        assertTrue(Files.isDirectory(commonSpec.resolve("server/languages/java/interfaces")));
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
            throw new UncheckedIOException(error);
        }
    }
}
