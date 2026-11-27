package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import java.util.Base64;
import java.util.List;

final class ZLinkRelocationCompletionRecordCodecTest {
    static final String ID = "0f1e2d3c-4b5a-4968-8778-a1b2c3d4e5f6";

    @Test
    void sharesCanonicalBytesChecksumsAndCountsWithTheOtherLanguages() throws Exception {
        try (var input = getClass().getResourceAsStream("/relocation-completion-record-v1.json")) {
            assertNotNull(input);
            for (var vector : new ObjectMapper().readTree(input).required("cases")) {
                String name = vector.required("name").textValue();
                byte[] bytes =
                        Base64.getDecoder().decode(vector.required("bytesBase64").textValue());
                if (vector.required("expect").textValue().equals("reject")) {
                    assertThrows(
                            ZLinkAggregateRelocationCoordinator.RelocationDataLostException.class,
                            () -> ZLinkRelocationCompletionRecordCodec.decode(bytes),
                            name);
                    continue;
                }
                var record = ZLinkRelocationCompletionRecordCodec.decode(bytes);
                assertEquals(ID, record.relocationId(), name);
                assertArrayEquals(bytes, ZLinkRelocationCompletionRecordCodec.encode(record), name);
                assertEquals(
                        vector.required("sha256").textValue(),
                        ZLinkRelocationCompletionRecordCodec.sha256(bytes),
                        name);
                assertEquals(
                        vector.required("completedResults").asInt(), record.entries().size(), name);
                assertEquals(
                        vector.required("pendingDeliveries").asLong(),
                        record.pendingDeliveries(),
                        name);
                if (name.equals("three-entries")) {
                    assertEquals(
                            List.of("pending", "terminalReceived", "sourceLeaseExpired"),
                            record.entries().stream()
                                    .map(ZLinkRelocationCompletionRecordCodec.Entry::delivery)
                                    .toList());
                    assertArrayEquals(
                            new byte[] {1, 2, 111, 107}, record.entries().getFirst().reply());
                    assertEquals("18446744073709551615", record.entries().get(1).replyRouteId());
                }
            }
        }
    }
}
