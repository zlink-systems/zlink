package systems.zlink.framework.runtime.internal.locations;

public sealed interface ZLinkAuthorityWriteResult
    permits ZLinkAuthorityStored, ZLinkAuthorityDeleted,
        ZLinkAuthorityConflict, ZLinkAuthorityGenerationExhausted {
}
