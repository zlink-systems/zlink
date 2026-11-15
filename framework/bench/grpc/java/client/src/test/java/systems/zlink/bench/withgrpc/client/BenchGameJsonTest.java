/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import systems.zlink.bench.withgrpc.shared.BenchGameJson;
import systems.zlink.bench.withgrpc.shared.BenchGamePayload;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.nio.charset.StandardCharsets;
import java.nio.file.Path;

final class BenchGameJsonTest {
    public static void main(String[] args) throws Exception {
        ObjectMapper fixtureMapper = new ObjectMapper();
        Path fixture =
                java.util.stream.Stream.of(
                                Path.of("ladder-mixed-payload.json"),
                                Path.of("..", "ladder-mixed-payload.json"),
                                Path.of("..", "..", "ladder-mixed-payload.json"))
                        .filter(path -> path.toFile().isFile())
                        .findFirst()
                        .orElseThrow(
                                () -> new IllegalStateException("shared JSON fixture not found"));
        JsonNode root = fixtureMapper.readTree(fixture.toFile());
        ZLinkJsonMessageSerializer frameworkJson = new ZLinkJsonMessageSerializer();

        for (String size : new String[] {"1024", "4096"}) {
            JsonNode golden = root.path("golden").path(size);
            byte[] expected = golden.path("json").asText().getBytes(StandardCharsets.UTF_8);
            BenchGamePayload payload =
                    BenchGamePayload.create(
                            Integer.parseInt(size), 123, (byte) 1, 456, 1_700_000_000_000_000_000L);

            assertBytes(expected, BenchGameJson.encode(payload), "raw JSON " + size);
            assertBytes(
                    expected,
                    frameworkJson.serialize(payload, BenchGamePayload.class).bytes(),
                    "framework typed JSON " + size);

            BenchGamePayload rawDecoded = BenchGameJson.decode(expected);
            BenchGamePayload frameworkDecoded =
                    frameworkJson.deserialize(
                            ZLinkEncodedPayload.from(expected), BenchGamePayload.class);
            assertStamp(rawDecoded, golden);
            assertStamp(frameworkDecoded, golden);
        }
    }

    private static void assertStamp(BenchGamePayload payload, JsonNode golden) {
        assertEquals(golden.path("run").asLong(), payload.run());
        assertEquals(golden.path("phase").asLong(), payload.phase());
        assertEquals(golden.path("seq").asLong(), payload.seq());
    }

    private static void assertBytes(byte[] expected, byte[] actual, String label) {
        if (!java.util.Arrays.equals(expected, actual)) {
            throw new AssertionError(label + " did not match fixture golden bytes");
        }
    }

    private static void assertEquals(Object expected, Object actual) {
        if (!java.util.Objects.equals(expected, actual)) {
            throw new AssertionError("expected " + expected + " but got " + actual);
        }
    }
}
