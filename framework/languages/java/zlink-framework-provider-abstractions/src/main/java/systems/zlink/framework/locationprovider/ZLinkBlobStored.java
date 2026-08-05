package systems.zlink.framework.locationprovider;

import java.time.Instant;

public record ZLinkBlobStored(Instant expiresAt, Instant storeNow)
    implements ZLinkBlobPutResult {}
