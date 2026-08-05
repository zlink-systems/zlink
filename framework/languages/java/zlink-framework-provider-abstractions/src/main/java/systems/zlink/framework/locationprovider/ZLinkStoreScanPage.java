package systems.zlink.framework.locationprovider;

import java.time.Instant;
import java.util.List;
import java.util.Objects;

public record ZLinkStoreScanPage(
    List<ZLinkStoreScanItem> items,
    ZLinkStoreScanCursor nextCursor,
    Instant storeNow) {
    public ZLinkStoreScanPage {
        items = List.copyOf(
            Objects.requireNonNull(items, "items"));
        Objects.requireNonNull(storeNow, "storeNow");
    }
}
