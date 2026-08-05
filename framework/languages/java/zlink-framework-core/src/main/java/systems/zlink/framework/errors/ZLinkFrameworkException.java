package systems.zlink.framework.errors;

public class ZLinkFrameworkException extends RuntimeException {
    private final ZLinkFrameworkErrorKind kind;

    public ZLinkFrameworkException(ZLinkFrameworkErrorKind kind, String message) {
        this(kind, message, null);
    }

    public ZLinkFrameworkException(ZLinkFrameworkErrorKind kind, String message, Throwable cause) {
        super(message, cause);
        this.kind = kind == null
            ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE
            : kind;
    }

    public ZLinkFrameworkErrorKind kind() {
        return kind;
    }

}
