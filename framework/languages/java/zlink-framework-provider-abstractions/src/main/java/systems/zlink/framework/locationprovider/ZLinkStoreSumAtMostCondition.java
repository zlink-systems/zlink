package systems.zlink.framework.locationprovider;

import java.math.BigInteger;
import java.util.List;
import java.util.Objects;
import java.util.function.Function;

public record ZLinkStoreSumAtMostCondition(List<ZLinkStoreKey> keys, long delta, long limit)
        implements ZLinkStoreCondition {
    public ZLinkStoreSumAtMostCondition {
        keys = List.copyOf(Objects.requireNonNull(keys, "keys"));
    }

    /** Evaluates this condition against a provider's transaction snapshot. Null denotes Missing. */
    public boolean matches(Function<ZLinkStoreKey, byte[]> snapshot) {
        BigInteger total = BigInteger.valueOf(delta);
        for (var key : keys) {
            total =
                    total.add(
                            BigInteger.valueOf(
                                    ZLinkStoreCounterDecimal.decode(snapshot.apply(key))));
        }
        return total.compareTo(BigInteger.valueOf(limit)) <= 0;
    }
}
