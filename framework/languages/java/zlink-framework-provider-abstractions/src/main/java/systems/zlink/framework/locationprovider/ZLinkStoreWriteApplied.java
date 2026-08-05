package systems.zlink.framework.locationprovider;

import java.time.Instant;
import java.util.Map;
import java.util.Objects;

public record ZLinkStoreWriteApplied(
    Map<ZLinkStoreKey, ZLinkStoreVersion> putVersions,
    Instant storeNow)
    implements ZLinkStoreWriteResult {
    public ZLinkStoreWriteApplied {
        putVersions = Map.copyOf(
            Objects.requireNonNull(putVersions, "putVersions"));
        Objects.requireNonNull(storeNow, "storeNow");
    }
}
