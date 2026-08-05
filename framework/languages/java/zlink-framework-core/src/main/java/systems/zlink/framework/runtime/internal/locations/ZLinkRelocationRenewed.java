package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;

public record ZLinkRelocationRenewed(
    Instant expiresAt,
    Instant storeNow)
    implements ZLinkRelocationRenewResult {
}
