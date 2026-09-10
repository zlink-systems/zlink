package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;

import java.util.UUID;
import org.junit.jupiter.api.Test;

final class ZLinkServiceOperationIdsTest {
    @Test
    void processPrefixIsStableAndCounterIsUnique() {
        UUID first = ZLinkServiceOperationIds.next();
        UUID second = ZLinkServiceOperationIds.next();

        assertEquals(
            first.getMostSignificantBits(),
            second.getMostSignificantBits());
        assertNotEquals(
            first.getLeastSignificantBits(),
            second.getLeastSignificantBits());
    }

    @Test
    void correlationIdUsesFixedLowercaseHexForBothWords() {
        assertEquals(
            "8000000000000000ffffffffffffffff",
            ZLinkServiceOperationIds.correlationId(
                new UUID(Long.MIN_VALUE, -1)));
    }
}
