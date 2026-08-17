package systems.zlink.framework.runtime.messaging;

import java.util.Map;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/**
 * Framework-origin marker for framework-generated errors. Only errors the
 * framework creates itself (route resolution failures, sealed admission,
 * dispatch rejections, transport terminals) carry reply metadata
 * {@code zlink.origin=framework}; errors an application handler raised never
 * do. Stale-route control reads the marker so a preserved application
 * {@code NotFound} kind is not mistaken for a stale route.
 */
public final class ZLinkFrameworkErrorOrigin {
    public static final String METADATA_KEY = "zlink.origin";
    public static final String FRAMEWORK = "framework";
    private static final Map<String, String> FRAMEWORK_METADATA =
        Map.of(METADATA_KEY, FRAMEWORK);

    private ZLinkFrameworkErrorOrigin() {
    }

    public static Map<String, String> frameworkMetadata() {
        return FRAMEWORK_METADATA;
    }

    public static ZLinkFrameworkException framework(
        ZLinkFrameworkErrorKind kind,
        String message) {
        return framework(kind, message, null);
    }

    public static ZLinkFrameworkException framework(
        ZLinkFrameworkErrorKind kind,
        String message,
        Throwable cause) {
        return new ZLinkFrameworkException(kind, message, cause, FRAMEWORK_METADATA);
    }

    public static boolean isFramework(Throwable error) {
        return error instanceof ZLinkFrameworkException framework
            && FRAMEWORK.equals(framework.metadata().get(METADATA_KEY));
    }
}
