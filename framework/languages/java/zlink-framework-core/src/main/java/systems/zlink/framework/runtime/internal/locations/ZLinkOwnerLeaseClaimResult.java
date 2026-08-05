package systems.zlink.framework.runtime.internal.locations;

public sealed interface ZLinkOwnerLeaseClaimResult
    permits ZLinkOwnerLeaseClaimed,
        ZLinkOwnerLeaseClaimConflict,
        ZLinkOwnerLeaseGenerationExhausted {
}
