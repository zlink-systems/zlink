package systems.zlink.framework.locationprovider;

import java.time.Instant;
import java.util.Objects;

public record ZLinkBlobFound(
    byte[] bytes,
    Instant expiresAt,
    Instant storeNow)
    implements ZLinkBlobReadResult {
    public ZLinkBlobFound {
        bytes = Objects.requireNonNull(bytes, "bytes").clone();
        Objects.requireNonNull(expiresAt, "expiresAt");
        Objects.requireNonNull(storeNow, "storeNow");
    }

    @Override
    public byte[] bytes() {
        return bytes.clone();
    }
}
