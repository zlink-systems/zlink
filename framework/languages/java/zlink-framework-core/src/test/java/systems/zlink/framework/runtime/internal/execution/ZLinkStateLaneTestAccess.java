package systems.zlink.framework.runtime.internal.execution;

import java.util.concurrent.Executor;

public final class ZLinkStateLaneTestAccess {
    private ZLinkStateLaneTestAccess() {}

    public static int queuedTurnCount(ZLinkStateLane lane) {
        return lane.queuedTurnCountForTest();
    }

    public static Executor replaceExecutor(ZLinkStateLane lane, Executor executor) {
        Executor previous = lane.executor;
        lane.executor = executor;
        return previous;
    }
}
