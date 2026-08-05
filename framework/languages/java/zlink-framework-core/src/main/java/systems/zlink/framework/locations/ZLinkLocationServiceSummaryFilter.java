package systems.zlink.framework.locations;

import systems.zlink.framework.runtime.internal.locations.*;

public record ZLinkLocationServiceSummaryFilter(
    String meshName) {

    public static ZLinkLocationServiceSummaryFilter all() {
        return new ZLinkLocationServiceSummaryFilter(null);
    }
}
