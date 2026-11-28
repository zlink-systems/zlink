package systems.zlink.framework.runtime.internal.execution;

public final class ZLinkStateLaneTestAccess {
    private ZLinkStateLaneTestAccess() {}

    public static int queuedTurnCount(ZLinkStateLane lane) {
        return lane.queuedTurnCountForTest();
    }

    public static String describeActiveTurn(ZLinkStateLane lane) {
        var turn = lane.activeTurnForTest();
        if (turn == null) return "ownerWorkKind=none ownerWorkStartedNanos=none";
        return "ownerWorkKind="
                + (turn.application() ? "application" : "infrastructure")
                + " ownerWorkStartedNanos="
                + turn.startedNanos();
    }
}
