package systems.zlink.framework.errors;

public final class ZLinkOperationCanceledException extends ZLinkFrameworkException {
    public ZLinkOperationCanceledException(String message) {
        super(ZLinkFrameworkErrorKind.INVALID_OPERATION, message);
    }
}
