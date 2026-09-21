package systems.zlink.stream.connector;

import java.util.Objects;

/**
 * The single exception type the connector throws, and the single type it fails a {@link
 * java.util.concurrent.CompletionStage} with.
 *
 * <p>Common connector spec §9.2 requires that the receiving side be able to read which of the
 * thirteen {@link ZLinkStreamErrorCode} values a failure carries. A language standard exception
 * such as {@code IllegalArgumentException} has nowhere to put that code, so a caller cannot tell
 * {@code VALIDATION_FAILED} from {@code CONFIGURATION_ERROR}. {@link #error()} is where the code
 * lives.
 */
public final class ZLinkStreamException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    private final transient ZLinkStreamError error;

    public ZLinkStreamException(ZLinkStreamError error) {
        super(Objects.requireNonNull(error, "error").message(), error.exception());
        this.error = error;
    }

    /** The full error, including the originating exception when there is one. */
    public ZLinkStreamError error() {
        return error;
    }

    /** Shorthand for {@code error().code()}. */
    public ZLinkStreamErrorCode errorCode() {
        return error.code();
    }

    static ZLinkStreamException of(ZLinkStreamErrorCode code, String message) {
        return new ZLinkStreamException(new ZLinkStreamError(code, message));
    }

    static ZLinkStreamException of(ZLinkStreamErrorCode code, String message, Throwable cause) {
        return new ZLinkStreamException(new ZLinkStreamError(code, message, cause));
    }

    static ZLinkStreamException validationFailed(String message) {
        return of(ZLinkStreamErrorCode.VALIDATION_FAILED, message);
    }

    static ZLinkStreamException validationFailed(String message, Throwable cause) {
        return of(ZLinkStreamErrorCode.VALIDATION_FAILED, message, cause);
    }

    static ZLinkStreamException configurationError(String message) {
        return of(ZLinkStreamErrorCode.CONFIGURATION_ERROR, message);
    }

    static ZLinkStreamException configurationError(String message, Throwable cause) {
        return of(ZLinkStreamErrorCode.CONFIGURATION_ERROR, message, cause);
    }

    static ZLinkStreamException disconnected(String message) {
        return of(ZLinkStreamErrorCode.DISCONNECTED, message);
    }
}
