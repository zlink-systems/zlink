package systems.zlink.framework.errors;

import java.util.Map;

public class ZLinkFrameworkException extends RuntimeException {
    private final ZLinkFrameworkErrorKind kind;
    private final Map<String, String> metadata;

    public ZLinkFrameworkException(ZLinkFrameworkErrorKind kind, String message) {
        this(kind, message, null);
    }

    public ZLinkFrameworkException(ZLinkFrameworkErrorKind kind, String message, Throwable cause) {
        this(kind, message, cause, Map.of());
    }

    public ZLinkFrameworkException(
        ZLinkFrameworkErrorKind kind,
        String message,
        Throwable cause,
        Map<String, String> metadata) {
        super(message, cause);
        this.kind = kind == null
            ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE
            : kind;
        this.metadata = metadata == null || metadata.isEmpty()
            ? Map.of()
            : Map.copyOf(metadata);
    }

    public ZLinkFrameworkErrorKind kind() {
        return kind;
    }

    /**
     * Framework-owned reply metadata carried with the error. Framework-generated
     * errors mark their origin here ({@code zlink.origin=framework}); errors an
     * application handler raised never carry that marker.
     */
    public Map<String, String> metadata() {
        return metadata;
    }

}
