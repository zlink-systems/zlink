package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import java.util.Objects;

public record ZLinkRelocationFound(byte[] payload, Instant expiresAt, Instant storeNow)
        implements ZLinkRelocationReadResult {
    public ZLinkRelocationFound {
        payload = Objects.requireNonNull(payload, "payload").clone();
        Objects.requireNonNull(expiresAt, "expiresAt");
        Objects.requireNonNull(storeNow, "storeNow");
    }

    @Override
    public byte[] payload() {
        return payload.clone();
    }
}
