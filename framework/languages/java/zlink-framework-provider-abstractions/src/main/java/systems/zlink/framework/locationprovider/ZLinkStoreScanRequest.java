package systems.zlink.framework.locationprovider;

public record ZLinkStoreScanRequest(
    String prefix,
    ZLinkStoreScanCursor cursor,
    int limit) {}
