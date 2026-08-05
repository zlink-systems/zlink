package systems.zlink.framework.locationprovider;

public sealed interface ZLinkStoreWriteResult
    permits ZLinkStoreWriteApplied, ZLinkStoreWriteConflict {}
