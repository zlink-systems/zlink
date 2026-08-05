package systems.zlink.framework.errors;

public final class ZLinkConfigurationException extends ZLinkFrameworkException {
    public ZLinkConfigurationException(String message) {
        super(ZLinkFrameworkErrorKind.NOT_CONFIGURED, message);
    }

    public ZLinkConfigurationException(
        ZLinkFrameworkErrorKind kind,
        String message) {
        super(kind, message);
    }

    public ZLinkConfigurationException(String message, Throwable cause) {
        super(ZLinkFrameworkErrorKind.NOT_CONFIGURED, message, cause);
    }
}
