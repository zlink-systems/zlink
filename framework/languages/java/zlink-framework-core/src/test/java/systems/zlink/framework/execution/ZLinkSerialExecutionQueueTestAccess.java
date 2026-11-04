package systems.zlink.framework.execution;

public final class ZLinkSerialExecutionQueueTestAccess {
    private ZLinkSerialExecutionQueueTestAccess() {}

    public static Object activeRecord(ZLinkSerialExecutionQueue queue) {
        return queue.active;
    }

    public static Object relocationState(ZLinkSerialExecutionQueue queue) {
        return queue.relocation;
    }
}
