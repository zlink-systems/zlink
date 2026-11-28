package systems.zlink.stream.connector;

import java.nio.file.Files;
import java.nio.file.Path;

final class ConnectorTestFixtures {
    private ConnectorTestFixtures() {}

    static Path file(String name) {
        Path relative = Path.of("framework/runtime/conformance").resolve(name);
        for (Path directory = Path.of("").toAbsolutePath();
                directory != null;
                directory = directory.getParent()) {
            Path fixture = directory.resolve(relative);
            if (Files.isRegularFile(fixture)) {
                return fixture;
            }
        }
        throw new AssertionError("Conformance fixture not found: " + relative);
    }
}
