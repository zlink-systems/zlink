package systems.zlink.framework.runtime.messaging;

import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.util.Map;

/**
 * Framework-origin marker for framework-generated errors. Only errors the framework creates itself
 * (route resolution failures, sealed admission, dispatch rejections, transport terminals) carry
 * reply metadata {@code zlink.origin=framework}; errors an application handler raised never do.
 * Stale-route control reads the marker so a preserved application {@code NotFound} kind is not
 * mistaken for a stale route.
 */
public final class ZLinkFrameworkErrorOrigin {
    public static final String METADATA_KEY = "zlink.origin";
    public static final String FRAMEWORK = "framework";
    private static final Map<String, String> FRAMEWORK_METADATA = Map.of(METADATA_KEY, FRAMEWORK);

    private ZLinkFrameworkErrorOrigin() {}

    public static Map<String, String> frameworkMetadata() {
        return FRAMEWORK_METADATA;
    }

    public static ZLinkFrameworkException framework(ZLinkFrameworkErrorKind kind, String message) {
        return framework(kind, message, (Throwable) null);
    }

    public static ZLinkFrameworkException framework(
            ZLinkFrameworkErrorKind kind, String message, Throwable cause) {
        if ((message == null || message.isBlank())
                && cause instanceof ZlinkSubmitException submit) {
            message = submitFailureMessage(submit);
        }
        return new ZLinkFrameworkException(kind, message, cause, FRAMEWORK_METADATA);
    }

    private static String submitFailureMessage(ZlinkSubmitException submit) {
        String reason =
                switch (submit.getResult()) {
                    case NOT_CONNECTED -> "failed because the target route is not connected.";
                    case NOT_FOUND -> "failed because the target was not found.";
                    case BACKPRESSURED -> "failed because submission capacity is unavailable.";
                    case NOT_ADMITTED -> "failed because the target did not admit the operation.";
                    case TERMINATED -> "failed because the runtime is shutting down.";
                    case INVALID_STATE ->
                            "failed because the operation is invalid in the current state.";
                    case INVALID_ARGUMENT -> "failed because an argument is invalid.";
                    case INVALID_HANDLE -> "failed because the handle is invalid.";
                    case THREAD_VIOLATION ->
                            "failed because the operation violates thread ownership.";
                    case NOT_SUPPORTED -> "failed because the operation is not supported.";
                    case OUT_OF_MEMORY -> "failed because memory is unavailable.";
                    case SEQ_EXHAUSTED -> "failed because the sequence is exhausted.";
                    case INTERNAL_ERROR -> "failed because of an internal binding error.";
                    case OK -> "the binding reported success on a failed submission.";
                };
        return "Submission failed with result "
                + submit.getResult()
                + (submit.getNativeErrno() == 0 ? "" : " (errno=" + submit.getNativeErrno() + ")")
                + ": "
                + reason;
    }

    public static ZLinkFrameworkException framework(
            ZLinkFrameworkErrorKind kind, String message, int failureCode) {
        return systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping
                .receivedFailure(kind, message, failureCode, FRAMEWORK_METADATA);
    }

    /**
     * The target refuses a message routed with an owner fence that is no longer current, before
     * admission. The reply carries {@code spotMoving}, which tells the caller that the message was
     * never accepted; the cause holds that code so the reported error stays a Framework error.
     */
    public static ZLinkFrameworkException ownerFenceRefusal(String message) {
        return framework(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                message,
                framework(
                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                        message,
                        (int)
                                systems.zlink.framework.runtime.protocol.ServiceWireConstants
                                        .FRAMEWORK_ERROR_SPOT_MOVING));
    }

    public static boolean isFramework(Throwable error) {
        return error instanceof ZLinkFrameworkException framework
                && FRAMEWORK.equals(framework.metadata().get(METADATA_KEY));
    }
}
