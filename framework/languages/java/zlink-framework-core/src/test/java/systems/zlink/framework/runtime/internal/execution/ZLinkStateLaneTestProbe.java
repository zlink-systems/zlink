package systems.zlink.framework.runtime.internal.execution;

public final class ZLinkStateLaneTestProbe {
    private ZLinkStateLaneTestProbe() {}

    public static int queuedTurnCount(ZLinkStateLane lane) {
        return lane.queuedTurnCountForTest();
    }

    public static final class Mailbox {
        private final java.util.concurrent.ConcurrentLinkedQueue<ZLinkStateLane.WorkItem> queue =
                new java.util.concurrent.ConcurrentLinkedQueue<>();
        private final ZLinkStateLane lane;

        public Mailbox(java.util.concurrent.Executor executor) {
            lane = new ZLinkStateLane(executor, queue);
        }

        public ZLinkStateLane lane() {
            return lane;
        }

        public void clear() {
            queue.clear();
        }
    }
}
