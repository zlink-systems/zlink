package systems.zlink.framework.locationprovider;

import java.time.Instant;
import java.util.Objects;

public record ZLinkStoreValue(
    byte[] bytes,
    ZLinkStoreVersion version,
    Instant expiresAt,
    Instant storeNow) {
    public ZLinkStoreValue {
        bytes = Objects.requireNonNull(bytes, "bytes").clone();
        Objects.requireNonNull(version, "version");
        Objects.requireNonNull(storeNow, "storeNow");
    }

    @Override
    public byte[] bytes() {
        return bytes.clone();
    }
}
