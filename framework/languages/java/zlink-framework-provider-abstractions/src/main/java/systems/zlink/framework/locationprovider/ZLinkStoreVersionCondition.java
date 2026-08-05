package systems.zlink.framework.locationprovider;

public record ZLinkStoreVersionCondition(
    ZLinkStoreKey key,
    ZLinkStoreVersion expected)
    implements ZLinkStoreCondition {}
