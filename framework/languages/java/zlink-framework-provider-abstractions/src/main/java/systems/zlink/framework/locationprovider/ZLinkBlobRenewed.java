package systems.zlink.framework.locationprovider;

import java.time.Instant;

public record ZLinkBlobRenewed(Instant expiresAt, Instant storeNow)
    implements ZLinkBlobRenewResult {}
