package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.testutil.PerCallAllocation;

import java.util.UUID;

final class ZLinkServiceOperationIdsTest {
    @Test
    void processPrefixIsStableAndCounterIsUnique() {
        UUID first = ZLinkServiceOperationIds.next();
        UUID second = ZLinkServiceOperationIds.next();

        assertEquals(first.getMostSignificantBits(), second.getMostSignificantBits());
        assertNotEquals(first.getLeastSignificantBits(), second.getLeastSignificantBits());
    }

    @Test
    void correlationIdUsesFixedLowercaseHexForBothWords() {
        assertEquals(
                "8000000000000000ffffffffffffffff",
                ZLinkServiceOperationIds.correlationId(new UUID(Long.MIN_VALUE, -1)));
        assertEquals(
                "0123456789abcdeffedcba9876543210",
                ZLinkServiceOperationIds.correlationId(
                        new UUID(0x0123456789abcdefL, 0xfedcba9876543210L)));
        assertEquals("0".repeat(32), ZLinkServiceOperationIds.correlationId(new UUID(0, 0)));
        assertEquals(
                "00000000000000010000000000000002",
                ZLinkServiceOperationIds.correlationId(new UUID(1, 2)));
    }

    @Test
    void correlationIdBuildsOneStringWithoutHexHalfStrings() {
        UUID id = new UUID(0x0123456789abcdefL, 0xfedcba9876543210L);
        int calls = 2_000;
        long bytes =
                PerCallAllocation.minWindow(
                        () -> ZLinkServiceOperationIds.correlationId(id), calls, 20_000);
        // One 16-byte scratch array and the final 32-character String. Two 16-digit half
        // strings plus their concatenation allocate about 184 bytes per id.
        assertTrue(
                bytes <= 112L * calls,
                "correlationId allocated " + bytes / calls + " bytes/id; budget 112");
    }
}
