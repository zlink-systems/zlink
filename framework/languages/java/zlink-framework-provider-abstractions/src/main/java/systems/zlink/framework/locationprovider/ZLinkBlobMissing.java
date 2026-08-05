package systems.zlink.framework.locationprovider;

import java.time.Instant;

public record ZLinkBlobMissing(Instant storeNow)
    implements ZLinkBlobReadResult {}
