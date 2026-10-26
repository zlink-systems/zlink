package systems.zlink.framework.runtime.internal.calls;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkOneWayFailureObservationTest {
    @Test
    void throwingObserverCannotLeaveTheCallerPending() {
        var physical = new CompletableFuture<Void>();
        var failure = new IllegalStateException("physical failure");
        var caller = ZLinkOneWayCalls.adaptOneWay(physical, false, ignored -> {
            throw new IllegalArgumentException("observer failure");
        }).toCompletableFuture();
        physical.completeExceptionally(failure);
        assertTrue(caller.isCompletedExceptionally());
        var result = assertThrows(java.util.concurrent.CompletionException.class, caller::join);
        assertSame(failure, result.getCause());
    }

    @Test
    void callerCancellationDoesNotSuppressTheMappedPhysicalFailure() {
        var physical = new CompletableFuture<Void>();
        var observed = new AtomicReference<Throwable>();
        var caller = ZLinkOneWayCalls.adaptOneWay(physical, false, failure -> {
            assertTrue(observed.compareAndSet(null, failure));
        }).toCompletableFuture();
        assertTrue(caller.cancel(false));
        assertFalse(physical.isDone());
        var failure = new ZlinkSubmitException(SubmitResult.NOT_FOUND);
        physical.completeExceptionally(failure);
        assertTrue(caller.isCancelled());
        var framework = assertInstanceOf(ZLinkFrameworkException.class, observed.get());
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, framework.kind());
        assertSame(failure, framework.getCause());
    }
}
