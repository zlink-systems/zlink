package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.nio.charset.StandardCharsets;
import java.util.Map;

final class ZLinkStreamJsonProfileBoundaryTest {
    record Counters(long min, long max, int small) {}

    record Ratio(double value) {}

    @Test
    void defaultCodecRejectsNonfiniteNumbers() {
        assertThrows(
                IllegalArgumentException.class,
                () -> ZLinkStreamJson.encode("ratio", new Ratio(Double.NaN)));
        ZLinkStreamEncodedPayload payload =
                new ZLinkStreamEncodedPayload(
                        "ratio",
                        Message.from("{\"value\":1e400}".getBytes(StandardCharsets.UTF_8)),
                        Map.of(),
                        ZLinkStreamCodec.JSON);
        assertThrows(
                IllegalArgumentException.class, () -> ZLinkStreamJson.decode(payload, Ratio.class));
    }

    @Test
    void defaultCodecPreservesSignedBoundariesAndInt32Numbers() {
        Counters expected = new Counters(Long.MIN_VALUE, Long.MAX_VALUE, Integer.MAX_VALUE);
        ZLinkStreamEncodedPayload encoded = ZLinkStreamJson.codec().encode("counters", expected);
        assertEquals(
                "{\"min\":\"-9223372036854775808\",\"max\":\"9223372036854775807\",\"small\":2147483647}",
                new String(encoded.payload().toByteArray(), StandardCharsets.UTF_8));
        assertEquals(expected, ZLinkStreamJson.codec().decode(encoded, Counters.class));
        ZLinkJsonMessageSerializer server = new ZLinkJsonMessageSerializer();
        assertEquals(
                expected,
                server.deserialize(
                        ZLinkEncodedPayload.from(encoded.payload().toByteArray()), Counters.class));
        ZLinkStreamEncodedPayload reply =
                new ZLinkStreamEncodedPayload(
                        "counters",
                        Message.from(server.serialize(expected).bytes()),
                        Map.of(),
                        ZLinkStreamCodec.JSON);
        assertEquals(expected, ZLinkStreamJson.codec().decode(reply, Counters.class));
    }

    @Test
    void defaultCodecRejectsInvalidIntegerStringsDuplicatePropertiesAndBom() {
        for (String json :
                new String[] {
                    "{\"min\":\"0001\",\"max\":\"1\",\"small\":1}",
                    "{\"min\":\"-0\",\"max\":\"1\",\"small\":1}",
                    "{\"min\":1,\"max\":\"1\",\"small\":1}",
                    "{\"min\":\"0\",\"min\":\"1\",\"max\":\"1\",\"small\":1}",
                    "\uFEFF{\"min\":\"0\",\"max\":\"1\",\"small\":1}",
                    "{\"MIN\":\"0\",\"max\":\"1\",\"small\":1}"
                }) {
            ZLinkStreamEncodedPayload payload =
                    new ZLinkStreamEncodedPayload(
                            "counters",
                            Message.from(json.getBytes(StandardCharsets.UTF_8)),
                            Map.of(),
                            ZLinkStreamCodec.JSON);
            assertThrows(
                    IllegalArgumentException.class,
                    () -> ZLinkStreamJson.codec().decode(payload, Counters.class),
                    json);
        }
    }
}
