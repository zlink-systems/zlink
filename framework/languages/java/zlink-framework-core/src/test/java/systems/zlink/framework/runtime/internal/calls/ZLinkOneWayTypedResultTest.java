package systems.zlink.framework.runtime.internal.calls;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;

final class ZLinkOneWayTypedResultTest {
    @ParameterizedTest
    @ValueSource(ints = {0, 101, 107, 111, 113, 10051, 10057, 10061, 10065})
    void nativeErrnoDoesNotReclassifyBindingAdmission(int errno) {
        var submission =
                CompletableFuture.<Void>failedFuture(
                        new ZlinkSubmitException(SubmitResult.NOT_ADMITTED, errno));
        var failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                ZLinkOneWayCalls.adaptOneWay(submission)
                                        .toCompletableFuture()
                                        .join());
        assertEquals(
                ZLinkFrameworkErrorKind.REJECTED,
                ((ZLinkFrameworkException) failure.getCause()).kind());
    }
}
