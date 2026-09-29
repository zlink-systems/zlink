package systems.zlink.framework.locationprovider;

public record ZLinkStoreValueCondition(ZLinkStoreKey key, byte[] expected)
        implements ZLinkStoreCondition {}
