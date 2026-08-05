package systems.zlink.framework.locationprovider;

public sealed interface ZLinkBlobPutResult
    permits ZLinkBlobStored, ZLinkBlobAlreadyStored, ZLinkBlobConflict {}
