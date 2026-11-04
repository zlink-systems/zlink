package systems.zlink.framework.runtime.internal.execution;

public final class ZLinkStateLaneTestAccess {
    private ZLinkStateLaneTestAccess() {}

    public static int queuedTurnCount(ZLinkStateLane lane) {
        return lane.queuedTurnCountForTest();
    }
}
