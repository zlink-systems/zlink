package systems.zlink.framework.locationprovider;

import java.time.Duration;
import java.util.Objects;

public record ZLinkStorePut(
    ZLinkStoreKey key,
    byte[] bytes,
    Duration retention)
    implements ZLinkStoreMutation {
    public ZLinkStorePut {
        Objects.requireNonNull(key, "key");
        bytes = Objects.requireNonNull(bytes, "bytes").clone();
    }

    @Override
    public byte[] bytes() {
        return bytes.clone();
    }
}
