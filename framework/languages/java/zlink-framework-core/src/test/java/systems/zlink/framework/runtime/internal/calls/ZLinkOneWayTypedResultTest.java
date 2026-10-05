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
    @org.junit.jupiter.api.Test
    void callerCancellationLeavesBindingAdmissionPending() {
        var binding = new CompletableFuture<Void>();
        var caller = ZLinkOneWayCalls.adaptOneWay(binding).toCompletableFuture();
        assertTrue(caller.cancel(false));
        assertFalse(binding.isDone());
        assertTrue(binding.complete(null));
    }

    @org.junit.jupiter.api.Test
    void pendingRouteRemovalIsUnavailable() {
        var binding = new CompletableFuture<Void>();
        var caller = ZLinkOneWayCalls.adaptOneWay(binding).toCompletableFuture();
        binding.completeExceptionally(new ZlinkSubmitException(SubmitResult.NOT_FOUND));
        var failure = assertThrows(CompletionException.class, caller::join);
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                ((ZLinkFrameworkException) failure.getCause()).kind());
    }

    @org.junit.jupiter.api.Test
    void sendConfigurationAndStreamModifierHaveNoTimeoutInput() {
        for (Class<?> type :
                new Class<?>[] {
                    systems.zlink.framework.configuration.ZLinkClientServerChannelClientBuilder
                            .class,
                    systems.zlink.framework.configuration.ZLinkMeshNodeSocketConfig.class,
                    systems.zlink.framework.channels.ZLinkSendCall.class,
                    systems.zlink.framework.spots.ZLinkSpotSendCall.class,
                    systems.zlink.framework.actors.ZLinkActorSendCall.class,
                    systems.zlink.framework.actors.ZLinkBoundSessionSendCall.class,
                    systems.zlink.framework.streams.ZLinkSessionActor.class,
                    systems.zlink.framework.streams.ZLinkSessionSendCall.class,
                    systems.zlink.framework.streams.ZLinkSessionReplyCall.class
                }) {
            for (var method : type.getMethods()) {
                assertFalse(
                        method.getName().equals("setSendTimeout")
                                || method.getName().equals("sendTimeout")
                                || method.getName().equals("timeout"),
                        type.getName());
            }
        }
    }

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
