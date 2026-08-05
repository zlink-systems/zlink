package systems.zlink.framework.locationprovider;

public sealed interface ZLinkBlobRenewResult
    permits ZLinkBlobRenewMissing, ZLinkBlobRenewed {}
