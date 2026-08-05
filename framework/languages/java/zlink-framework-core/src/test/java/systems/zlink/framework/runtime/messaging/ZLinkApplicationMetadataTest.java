package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.LinkedHashMap;
import java.util.Map;
import org.junit.jupiter.api.Test;

final class ZLinkApplicationMetadataTest {
    @Test
    void builderSnapshotsInputAndLastValueWins() {
        LinkedHashMap<String, String> source = new LinkedHashMap<>();
        source.put("trace-id", "first");
        ZLinkApplicationMetadata metadata =
            ZLinkApplicationMetadata.copyOf(source)
                .with("trace-id", "last")
                .withAll(Map.of("tenant", "blue"));

        source.put("trace-id", "mutated");

        Map<String, String> decoded =
            ZLinkApplicationMetadata.decode(metadata.encode());
        assertEquals(
            Map.of("trace-id", "last", "tenant", "blue"),
            decoded);
        assertThrows(
            UnsupportedOperationException.class,
            () -> decoded.put("late", "value"));
    }

    @Test
    void rejectsEmptyKeyOversizeAndMalformedFrames() {
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkApplicationMetadata.empty().with("", "value"));
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkApplicationMetadata.copyOf(
                Map.of("key", "x".repeat(1024))).encode());
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkApplicationMetadata.decode(
                new byte[] {1, 1, 1, 'k', 0, 1}));
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkApplicationMetadata.decode(
                new byte[] {1, 0, 1}));
    }
}
