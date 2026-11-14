package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;

import org.jspecify.annotations.Nullable;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;

import systems.zlink.framework.runtime.internal.json.ZLinkFrameworkJsonProfile;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.stream.Stream;

final class ZLinkTypedJsonPayloadContractTest {
    record ConstructorPayload(byte[] body) {}

    public static final class PropertyPayload {
        public byte[] body = new byte[0];
    }

    static Stream<Arguments> rows() throws Exception {
        Path root = Path.of("").toAbsolutePath();
        while (!Files.exists(root.resolve("framework/test/fixtures/typed-json-payload.tsv")))
            root = root.getParent();
        return Files.readAllLines(root.resolve("framework/test/fixtures/typed-json-payload.tsv"))
                .stream()
                .filter(line -> !line.startsWith("#") && !line.isEmpty())
                .map(
                        line -> {
                            String[] fields = line.split("\t", -1);
                            return Arguments.of(
                                    fields[0], fields[1].equals("accept"), fields[2], fields[3]);
                        });
    }

    record NullablePayload(byte @Nullable [] body) {}

    @Test
    void explicitlyNullableBytesAreAccepted() throws Exception {
        assertNull(
                ZLinkFrameworkJsonProfile.mapper()
                        .readValue("{\"body\":null}", NullablePayload.class)
                        .body());
    }

    @ParameterizedTest(name = "{0}")
    @MethodSource("rows")
    void sharedTypedContract(String id, boolean accept, String shape, String json) {
        boolean accepted;
        try {
            Class<?> type = shape.equals("ctor") ? ConstructorPayload.class : PropertyPayload.class;
            ZLinkFrameworkJsonProfile.mapper()
                    .readValue(json.getBytes(StandardCharsets.UTF_8), type);
            accepted = true;
        } catch (Exception error) {
            accepted = false;
        }
        assertEquals(accept, accepted, id);
    }
}
