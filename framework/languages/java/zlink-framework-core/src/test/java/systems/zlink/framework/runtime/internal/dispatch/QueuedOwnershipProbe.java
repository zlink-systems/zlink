package systems.zlink.framework.runtime.internal.dispatch;

/** Read-only access to ownership state for tests in other packages. */
public final class QueuedOwnershipProbe {
    private QueuedOwnershipProbe() {}

    public static boolean hasReservation(ZLinkApplicationJobContext.QueuedOwnership ownership) {
        return ownership.hasReservation();
    }
}
