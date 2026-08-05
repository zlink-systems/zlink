package systems.zlink.framework.errors;

/**
 * Raised when a CPU worker submit is rejected because the worker queue
 * is full. The submit fails immediately; the Spot dispatcher is never blocked
 * waiting for queue capacity.
 */
public class ZLinkWorkerQueueFullException extends ZLinkFrameworkException {
    public ZLinkWorkerQueueFullException(String message) {
        super(ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED, message);
    }
}
