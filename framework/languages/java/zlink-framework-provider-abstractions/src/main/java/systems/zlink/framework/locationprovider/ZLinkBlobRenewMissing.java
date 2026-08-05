package systems.zlink.framework.locationprovider;

import java.time.Instant;

public record ZLinkBlobRenewMissing(Instant storeNow)
    implements ZLinkBlobRenewResult {}
