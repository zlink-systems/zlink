package systems.zlink.framework.locationprovider;

import java.time.Instant;

public record ZLinkStoreWriteConflict(Instant storeNow)
    implements ZLinkStoreWriteResult {}
