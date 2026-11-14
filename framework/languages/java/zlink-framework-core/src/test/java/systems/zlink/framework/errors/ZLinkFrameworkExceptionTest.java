package systems.zlink.framework.errors;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.NullAndEmptySource;
import org.junit.jupiter.params.provider.ValueSource;

import java.util.Map;

final class ZLinkFrameworkExceptionTest {
    @Test
    void stringIncludesKindWithoutChangingApplicationMessageOrErrorDetails() {
        var cause = new IllegalStateException("application cause");
        var metadata = Map.of("application", "detail");
        var error =
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NOT_FOUND, "application message", cause, metadata);

        assertEquals("application message", error.getMessage());
        assertEquals(
                "systems.zlink.framework.errors.ZLinkFrameworkException [NOT_FOUND]: application message",
                error.toString());
        assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND, error.kind());
        assertEquals(metadata, error.metadata());
        assertSame(cause, error.getCause());
    }

    @ParameterizedTest
    @NullAndEmptySource
    @ValueSource(strings = {" \t"})
    void blankMessageStillShowsKindAndCause(String message) {
        var cause = new IllegalStateException("application cause");
        var error =
                new ZLinkFrameworkException(ZLinkFrameworkErrorKind.UNAVAILABLE, message, cause);

        assertEquals(message, error.getMessage());
        assertEquals(
                "systems.zlink.framework.errors.ZLinkFrameworkException [UNAVAILABLE]: caused by java.lang.IllegalStateException: application cause",
                error.toString());
        assertSame(cause, error.getCause());
    }

    @Test
    void missingKindAndMessageStillShowTheEffectiveKind() {
        var error = new ZLinkFrameworkException(null, null);
        assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, error.kind());
        assertNull(error.getMessage());
        assertEquals(
                "systems.zlink.framework.errors.ZLinkFrameworkException [INTERNAL_FAILURE]",
                error.toString());
    }
}
