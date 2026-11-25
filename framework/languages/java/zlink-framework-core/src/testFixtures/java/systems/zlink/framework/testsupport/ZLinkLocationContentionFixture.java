package systems.zlink.framework.testsupport;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

public final class ZLinkLocationContentionFixture {
    private static final String PATH = "runtime/conformance/location-contention-v1.json";

    private ZLinkLocationContentionFixture() {}

    public static JsonNode scenario(String name) throws IOException {
        for (Path directory = Path.of("").toAbsolutePath();
                directory != null;
                directory = directory.getParent()) {
            Path fixture = directory.resolve(PATH);
            if (Files.isRegularFile(fixture)) {
                return new ObjectMapper().readTree(fixture.toFile()).required(name);
            }
        }
        throw new IOException("Missing conformance fixture " + PATH);
    }
}
