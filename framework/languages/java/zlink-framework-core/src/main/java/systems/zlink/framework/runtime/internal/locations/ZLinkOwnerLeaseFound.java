package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.time.Instant;
import java.util.Objects;

public record ZLinkOwnerLeaseFound(
        ZLinkLocationOwnerToken token, Instant leaseExpiresAt, Instant storeNow)
        implements ZLinkOwnerLeaseReadResult {
    public ZLinkOwnerLeaseFound {
        Objects.requireNonNull(token, "token");
        if (leaseExpiresAt == null) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                    "owner lease Found result has no expiration time");
        }
        Objects.requireNonNull(storeNow, "storeNow");
    }
}
