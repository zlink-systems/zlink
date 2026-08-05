package systems.zlink.framework.runtime.internal.locations;

public sealed interface ZLinkObjectReserveResult
    permits ZLinkObjectReserved, ZLinkObjectConflict,
        ZLinkObjectAlreadyExists, ZLinkObjectTypeMismatch,
        ZLinkPlacementCapacityExhausted,
        ZLinkObjectGenerationExhausted {
}
