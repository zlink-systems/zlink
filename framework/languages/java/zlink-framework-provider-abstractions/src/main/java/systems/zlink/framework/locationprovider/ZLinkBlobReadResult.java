package systems.zlink.framework.locationprovider;

public sealed interface ZLinkBlobReadResult
    permits ZLinkBlobMissing, ZLinkBlobFound {}
