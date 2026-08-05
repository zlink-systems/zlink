package systems.zlink.framework.locationprovider;

public sealed interface ZLinkStoreScanResult
    permits ZLinkStoreScanPageResult, ZLinkStoreScanExpired {}
