package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.ReceivedTesting;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;

import java.util.List;
import java.util.Optional;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;

final class ZLinkJavaSpotAuthorityFailureTest {
    @Test
    void unexpectedFailureRetainsInternalFailureAndNextMessageIsChecked() throws Exception {
        verifyFailure(
                new IllegalStateException("spot authority failed"),
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                false);
    }

    @Test
    void cancellationRetainsShuttingDownAndNextMessageIsChecked() throws Exception {
        verifyFailure(
                new CancellationException("spot authority cancelled"),
                ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                false);
    }

    @Test
    void classifiedFailureRetainsDeadlineAndNextMessageIsChecked() throws Exception {
        verifyFailure(
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, "spot authority deadline"),
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                false);
    }

    @Test
    void providerArgumentFailureRetainsInternalFailure() throws Exception {
        verifyFailure(
                new IllegalArgumentException("spot authority argument failed"),
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                false);
    }

    @Test
    void synchronousFailureUsesTheSameMessageTerminal() throws Exception {
        verifyFailure(
                new IllegalStateException("synchronous spot authority failed"),
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                true);
    }

    @Test
    void synchronousCancellationUsesTheSameMessageTerminal() throws Exception {
        verifyFailure(
                new CancellationException("synchronous spot authority cancelled"),
                ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                true);
    }

    @Test
    void absentAuthorityRemainsUnavailable() throws Exception {
        try (var fixture = new ZLinkJavaActorAuthorityFailureTest.Fixture()) {
            var inbound = fixture.dispatchSpot(1);
            fixture.authority.complete(Optional.empty());
            int[] pair = ZLinkRequestFailureMapping.outgoing(ZLinkFrameworkErrorKind.UNAVAILABLE);
            assertEquals(
                    List.of(new ZLinkServiceM6AWireCodec.Reply(1, pair[0], pair[1])),
                    fixture.replies);
            assertTrue(ReceivedTesting.isClosed(inbound.received()));
            assertEquals(2, fixture.lookups.get());
        }
    }

    private static void verifyFailure(
            RuntimeException failure, ZLinkFrameworkErrorKind kind, boolean synchronous)
            throws Exception {
        try (var fixture = new ZLinkJavaActorAuthorityFailureTest.Fixture()) {
            fixture.synchronousFailure = synchronous ? failure : null;
            var failed = fixture.dispatchSpot(1);
            if (!synchronous) fixture.authority.completeExceptionally(failure);
            int[] pair = ZLinkRequestFailureMapping.outgoing(kind);
            assertEquals(
                    List.of(new ZLinkServiceM6AWireCodec.Reply(1, pair[0], pair[1])),
                    fixture.replies);
            assertTrue(ReceivedTesting.isClosed(failed.received()));
            fixture.synchronousFailure = null;
            fixture.authority = CompletableFuture.completedFuture(Optional.empty());
            var suffix = fixture.dispatchSpot(2);
            assertEquals(
                    synchronous ? 3 : 4,
                    fixture.lookups.get(),
                    "the next message still resolves both authorities");
            int[] unavailable =
                    ZLinkRequestFailureMapping.outgoing(ZLinkFrameworkErrorKind.UNAVAILABLE);
            assertEquals(
                    new ZLinkServiceM6AWireCodec.Reply(2, unavailable[0], unavailable[1]),
                    fixture.replies.get(1));
            assertTrue(ReceivedTesting.isClosed(suffix.received()));
        }
    }
}
