package systems.zlink.framework.locationprovider;

import java.time.Instant;

public record ZLinkStoreWriteCorrupt(Instant storeNow) implements ZLinkStoreWriteResult {}
