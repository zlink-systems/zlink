package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;

import java.util.UUID;

final class ZLinkRelocationReplyRowCodecTest {
    private static final String MAX_UINT64 = "18446744073709551615";

    @Test
    void roundTripsMaximumUnsignedLeaseGenerations() {
        var row =
                new ZLinkRelocationReplyRow(
                        new UUID(1, 2),
                        ZLinkAuthorityKeyCodec.actor("actor"),
                        new ZLinkRelocationReplyRow.Source("source", MAX_UINT64, "61", "1"),
                        "00000000000000000000000000000001",
                        "1",
                        new ZLinkLocationOwnerToken("writer", Long.parseUnsignedLong(MAX_UINT64)),
                        ZLinkRelocationReplyRow.ACCEPTED,
                        null);
        var decoded = ZLinkRelocationReplyRow.decode(row.key(), row.encode());
        assertEquals(MAX_UINT64, decoded.source().leaseGeneration());
        assertEquals(MAX_UINT64, Long.toUnsignedString(decoded.writer().leaseGeneration()));
    }

    @Test
    void rejectsZeroAndOutOfRangeSourceLeaseGenerations() {
        assertThrows(
                IllegalArgumentException.class,
                () -> new ZLinkRelocationReplyRow.Source("source", "0", "61", "1"));
        assertThrows(
                IllegalArgumentException.class,
                () ->
                        new ZLinkRelocationReplyRow.Source(
                                "source", "18446744073709551616", "61", "1"));
    }
}
