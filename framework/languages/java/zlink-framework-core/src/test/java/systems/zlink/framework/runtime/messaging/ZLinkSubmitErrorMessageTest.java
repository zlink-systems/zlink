package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;

final class ZLinkSubmitErrorMessageTest {
    @ParameterizedTest
    @ValueSource(ints = {0, 107, 10057})
    void missingBindingMessageIncludesResultReasonAndAvailableErrno(int errno) {
        var submit = new ZlinkSubmitException(SubmitResult.NOT_CONNECTED, errno);
        var error = adapt(submit);

        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, error.kind());
        assertEquals(
                "Submission failed with result NOT_CONNECTED"
                        + (errno == 0 ? "" : " (errno=" + errno + ")")
                        + ": failed because the target route is not connected.",
                error.getMessage());
        assertTrue(error.toString().contains("[UNAVAILABLE]"), error.toString());
        assertTrue(error.toString().contains("NOT_CONNECTED"), error.toString());
        assertSame(submit, error.getCause());
        assertEquals(ZLinkFrameworkErrorOrigin.frameworkMetadata(), error.metadata());
    }

    @ParameterizedTest
    @EnumSource(value = SubmitResult.class, names = "OK", mode = EnumSource.Mode.EXCLUDE)
    void everySubmitFailureHasADiagnosticMessage(SubmitResult result) {
        var error = adapt(new ZlinkSubmitException(result));
        assertNotNull(error.getMessage());
        assertFalse(error.getMessage().isBlank());
        assertTrue(error.getMessage().contains(result.name()), error.getMessage());
    }

    @Test
    void explicitFrameworkMessageIsPreserved() {
        var submit = new ZlinkSubmitException(SubmitResult.NOT_CONNECTED, 107);
        var error =
                ZLinkFrameworkErrorOrigin.framework(
                        ZLinkFrameworkErrorKind.UNAVAILABLE, "specific operation failed", submit);
        assertEquals("specific operation failed", error.getMessage());
        assertSame(submit, error.getCause());
    }

    @Test
    void applicationFailureIsNotWrappedOrRewritten() {
        var application =
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NOT_FOUND, "application handler failure");
        var failed =
                ZLinkOneWayCalls.adaptOneWay(CompletableFuture.<Void>failedFuture(application))
                        .toCompletableFuture();
        assertSame(application, assertThrows(CompletionException.class, failed::join).getCause());
        assertEquals("application handler failure", application.getMessage());
        assertTrue(application.metadata().isEmpty());
    }

    private static ZLinkFrameworkException adapt(ZlinkSubmitException submit) {
        var failed =
                ZLinkOneWayCalls.adaptOneWay(CompletableFuture.<Void>failedFuture(submit))
                        .toCompletableFuture();
        return assertInstanceOf(
                ZLinkFrameworkException.class,
                assertThrows(CompletionException.class, failed::join).getCause());
    }
}
