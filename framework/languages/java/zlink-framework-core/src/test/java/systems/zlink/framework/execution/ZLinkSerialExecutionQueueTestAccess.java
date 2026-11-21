package systems.zlink.framework.execution;

public final class ZLinkSerialExecutionQueueTestAccess {
    private ZLinkSerialExecutionQueueTestAccess() {}

    public static Object activeRecord(ZLinkSerialExecutionQueue queue) {
        return queue.active;
    }

    public static Object relocationState(ZLinkSerialExecutionQueue queue) {
        return queue.relocation;
    }

    public static void exhaustSequence(ZLinkSerialExecutionQueue queue) {
        synchronized (queue) {
            queue.nextSequence = Long.MAX_VALUE;
        }
    }

    /** Tells whether an accepted application entry waits in the queue, published or not. */
    public static boolean retainsApplication(ZLinkSerialExecutionQueue queue) {
        synchronized (queue) {
            return !queue.applicationPending.isEmpty() || !queue.publishedApplication.isEmpty();
        }
    }
}
