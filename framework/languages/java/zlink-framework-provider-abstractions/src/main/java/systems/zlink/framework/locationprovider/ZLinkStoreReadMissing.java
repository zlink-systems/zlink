package systems.zlink.framework.locationprovider;

import java.time.Instant;

public record ZLinkStoreReadMissing(Instant storeNow)
    implements ZLinkStoreReadResult {}
