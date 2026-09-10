package systems.zlink.framework.runtime.internal.service;

import java.util.HexFormat;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicLong;

/** Process-unique 128-bit identities without per-operation randomness. */
public final class ZLinkServiceOperationIds {
    private static final HexFormat HEX = HexFormat.of();
    private static final long PROCESS_PREFIX = processPrefix();
    private static final AtomicLong NEXT_COUNTER = new AtomicLong(1);

    private ZLinkServiceOperationIds() {
    }

    public static UUID next() {
        while (true) {
            long current = NEXT_COUNTER.get();
            if (current == 0) {
                throw new IllegalStateException(
                    "service operation identity space is exhausted");
            }
            long next = current == -1 ? 0 : current + 1;
            if (NEXT_COUNTER.compareAndSet(current, next)) {
                return new UUID(PROCESS_PREFIX, current);
            }
        }
    }

    public static String correlationId(UUID id) {
        UUID value = Objects.requireNonNull(id, "id");
        return HEX.toHexDigits(value.getMostSignificantBits())
            + HEX.toHexDigits(value.getLeastSignificantBits());
    }

    private static long processPrefix() {
        UUID seed = UUID.randomUUID();
        long prefix = seed.getMostSignificantBits() ^ seed.getLeastSignificantBits();
        return prefix == 0 ? 0x6a09e667f3bcc909L : prefix;
    }
}
