package systems.zlink.framework.runtime.internal.locations;

import java.util.List;
import java.util.Optional;

public record ZLinkAuthorityPage(
    List<ZLinkAuthorityEntry> items,
    Optional<ZLinkAuthorityScanCursor> nextCursor)
    implements ZLinkAuthorityScanResult {
    public ZLinkAuthorityPage {
        items = List.copyOf(items);
        nextCursor = java.util.Objects.requireNonNull(
            nextCursor,
            "nextCursor");
    }
}
