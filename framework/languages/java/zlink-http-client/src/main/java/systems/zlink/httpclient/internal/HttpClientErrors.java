/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.net.http.HttpTimeoutException;
import java.util.concurrent.TimeoutException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/** Owns the HTTP client contract's mapping from failure situations to framework error kinds. */
public final class HttpClientErrors {

    private HttpClientErrors() {
    }

    public static ZLinkFrameworkException protocol(String message) {
        return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, message);
    }

    public static ZLinkFrameworkException protocol(String message, Throwable cause) {
        return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, message, cause);
    }

    public static ZLinkFrameworkException unavailable(Throwable cause) {
        return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.UNAVAILABLE, cause.getMessage(), cause);
    }

    public static ZLinkFrameworkException rejected(String message) {
        return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.REJECTED, message);
    }

    public static ZLinkFrameworkException deadlineExceeded(Throwable cause) {
        return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, cause.getMessage(), cause);
    }

    public static ZLinkFrameworkException internalFailure(String message) {
        return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, message);
    }

    public static ZLinkFrameworkException internalFailure(Throwable cause) {
        return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, cause.getMessage(), cause);
    }

    public static ZLinkFrameworkException fromExecutionFailure(Throwable cause) {
        if (cause instanceof ZLinkFrameworkException failure) {
            return failure;
        }
        if (cause instanceof HttpTimeoutException || cause instanceof TimeoutException) {
            return deadlineExceeded(cause);
        }
        if (cause instanceof IOException || cause instanceof UncheckedIOException) {
            return unavailable(cause);
        }
        return internalFailure(cause);
    }
}
