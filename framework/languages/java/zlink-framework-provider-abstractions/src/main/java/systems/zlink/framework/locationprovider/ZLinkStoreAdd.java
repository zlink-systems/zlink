package systems.zlink.framework.locationprovider;

import java.nio.charset.StandardCharsets;
import java.util.Objects;

public record ZLinkStoreAdd(ZLinkStoreKey key, long delta) implements ZLinkStoreMutation {
    public ZLinkStoreAdd {
        Objects.requireNonNull(key, "key");
    }

    /** Resolves this mutation against a provider's transaction snapshot. Null denotes Missing. */
    public ZLinkStorePut resolve(byte[] current) {
        long result = Math.addExact(ZLinkStoreCounterDecimal.decode(current), delta);
        if (result < 0) {
            throw new ArithmeticException("Location Store counter underflow");
        }
        return new ZLinkStorePut(
                key, Long.toString(result).getBytes(StandardCharsets.US_ASCII), null);
    }
}
