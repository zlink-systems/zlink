package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import java.util.List;

public record ZLinkOwnerLeaseSnapshot(
    List<ZLinkOwnerLease> leases,
    Instant storeNow) {
}
