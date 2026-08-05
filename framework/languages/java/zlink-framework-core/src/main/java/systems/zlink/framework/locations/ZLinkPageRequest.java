package systems.zlink.framework.locations;

import systems.zlink.framework.runtime.internal.locations.*;

public record ZLinkPageRequest(
    int pageSize,
    String continuationToken) {

    public static ZLinkPageRequest firstPage() {
        return new ZLinkPageRequest(0, null);
    }
}
