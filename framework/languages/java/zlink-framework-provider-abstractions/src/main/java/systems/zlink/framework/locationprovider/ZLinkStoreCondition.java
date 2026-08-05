package systems.zlink.framework.locationprovider;

public sealed interface ZLinkStoreCondition
    permits ZLinkStoreMissingCondition, ZLinkStoreVersionCondition {}
