package systems.zlink.framework.runtime.internal.locations;

public sealed interface ZLinkRelocationCapacityReserveResult
    permits ZLinkRelocationCapacityReserved,
        ZLinkRelocationCapacityAlreadyReserved,
        ZLinkRelocationCapacityConflict,
        ZLinkRelocationCapacityTargetUnavailable,
        ZLinkRelocationCapacityExhausted {
}
