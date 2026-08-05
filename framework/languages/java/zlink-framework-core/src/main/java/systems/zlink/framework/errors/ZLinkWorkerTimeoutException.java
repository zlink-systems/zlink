package systems.zlink.framework.errors;

/**
 * Raised when a worker task does not complete before its timeout.
 * The caller completion fails with this exception; a late worker result is dropped
 * and never re-enters the Spot dispatcher.
 */
public class ZLinkWorkerTimeoutException extends ZLinkFrameworkException {
    public ZLinkWorkerTimeoutException(String message) {
        super(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, message);
    }
}
