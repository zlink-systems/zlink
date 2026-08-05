package systems.zlink.framework.locationprovider;

public sealed interface ZLinkStoreReadResult
    permits ZLinkStoreReadMissing, ZLinkStoreReadFound {}
