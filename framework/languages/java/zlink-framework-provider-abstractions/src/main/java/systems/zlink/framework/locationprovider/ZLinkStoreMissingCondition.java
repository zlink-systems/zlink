package systems.zlink.framework.locationprovider;

public record ZLinkStoreMissingCondition(ZLinkStoreKey key)
    implements ZLinkStoreCondition {}
