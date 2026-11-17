/* SPDX-License-Identifier: Elastic-2.0 */

package systems.zlink.bench.withgrpc.client;

import com.fasterxml.jackson.databind.JsonMappingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;

import systems.zlink.bench.withgrpc.shared.BenchGameJson;
import systems.zlink.bench.withgrpc.shared.BenchGamePayload;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

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
            assertBytes(expected, BenchGameJson.encode(rawDecoded), "binding decoded body " + size);
            assertBytes(
                    expected,
                    frameworkJson.serialize(frameworkDecoded, BenchGamePayload.class).bytes(),
                    "framework decoded body " + size);
            ObjectNode valid = (ObjectNode) fixtureMapper.readTree(expected);
            List<ObjectNode> invalid = new ArrayList<>();
            valid.fieldNames()
                    .forEachRemaining(
                            name -> {
                                ObjectNode missing = valid.deepCopy();
                                missing.remove(name);
                                invalid.add(missing);
                                ObjectNode nullField = valid.deepCopy();
                                nullField.putNull(name);
                                invalid.add(nullField);
                            });
            valid.path("items")
                    .path(0)
                    .fieldNames()
                    .forEachRemaining(
                            name -> {
                                ObjectNode missing = valid.deepCopy();
                                ((ObjectNode) missing.path("items").path(0)).remove(name);
                                invalid.add(missing);
                                ObjectNode nullField = valid.deepCopy();
                                ((ObjectNode) nullField.path("items").path(0)).putNull(name);
                                invalid.add(nullField);
                            });
            ObjectNode noncanonical = valid.deepCopy();
            noncanonical.put("gold", "01");
            invalid.add(noncanonical);
            ObjectNode numeric = valid.deepCopy();
            numeric.put("gold", 1);
            invalid.add(numeric);
            ObjectNode stringHeading = valid.deepCopy();
            stringHeading.put("heading", "1");
            // The current Framework profile delegates int32 coercion to Jackson.
            // This case checks PM parity; missing/null fields below must both reject.
            byte[] stringHeadingBytes = fixtureMapper.writeValueAsBytes(stringHeading);
            BenchGamePayload rawStringHeading = BenchGameJson.decode(stringHeadingBytes);
            BenchGamePayload frameworkStringHeading =
                    frameworkJson.deserialize(
                            ZLinkEncodedPayload.from(stringHeadingBytes), BenchGamePayload.class);
            assertEquals(1, rawStringHeading.heading());
            assertEquals(rawStringHeading, frameworkStringHeading);
            for (ObjectNode bad : invalid) {
                byte[] bytes = fixtureMapper.writeValueAsBytes(bad);
                Throwable rawError = rejection(() -> BenchGameJson.decode(bytes), "binding " + bad);
                Throwable frameworkError =
                        rejection(
                                () ->
                                        frameworkJson.deserialize(
                                                ZLinkEncodedPayload.from(bytes),
                                                BenchGamePayload.class),
                                "framework " + bad);
                if (!(rawError.getCause() instanceof JsonMappingException)
                        || rawError.getCause().getClass() != frameworkError.getCause().getClass()) {
                    throw new AssertionError("JSON mapping error categories differ", rawError);
                }
            }
            System.out.println("Java fixture " + size + " bytes/decode/rejection parity PASS");
        }
    }

    private static Throwable rejection(Runnable decode, String label) {
        try {
            decode.run();
        } catch (IllegalArgumentException error) {
            return error;
        }
        throw new AssertionError("invalid JSON contract was accepted: " + label);
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
