package systems.zlink.framework.locationprovider;

import java.time.Instant;

public record ZLinkBlobConflict(Instant storeNow)
    implements ZLinkBlobPutResult {}
