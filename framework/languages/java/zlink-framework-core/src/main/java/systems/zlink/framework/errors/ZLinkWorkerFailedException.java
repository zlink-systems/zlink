package systems.zlink.framework.errors;

/**
 * Raised when a worker task fails. The original failure is
 * preserved as the cause.
 */
public class ZLinkWorkerFailedException extends ZLinkFrameworkException {
    public ZLinkWorkerFailedException(String message, Throwable cause) {
        super(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, message, cause);
    }
}
