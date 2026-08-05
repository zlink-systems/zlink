package systems.zlink.framework.locationprovider;

import java.time.Instant;

public record ZLinkBlobAlreadyStored(Instant expiresAt, Instant storeNow)
    implements ZLinkBlobPutResult {}
