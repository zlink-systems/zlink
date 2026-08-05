package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkAuthorityEntry(
    String key,
    ZLinkAuthoritySnapshot snapshot) {
    public ZLinkAuthorityEntry {
        Objects.requireNonNull(key, "key");
        Objects.requireNonNull(snapshot, "snapshot");
    }
}
