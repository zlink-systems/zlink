package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode.CanonicalActorJoinResponse;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

final class ZLinkJavaConnectionObserverTest {
    @BeforeAll
    static void enableFileLog() throws Exception {
        ZLinkJavaOwnerDescriptorWaitTest.enableFileLog();
    }

    @AfterAll
    static void closeFileLog() {
        ZLinkJavaOwnerDescriptorWaitTest.closeFileLog();
    }

    @Test
    void actorJoinCompletesWhileConnectionObserverIsBlocked() throws Exception {
        var targetEntered = new CompletableFuture<Void>();
        var targetResponse = new CompletableFuture<CanonicalActorJoinResponse>();
        try (var fixture = new ZLinkJavaOwnerDescriptorWaitTest.Fixture();
                var listener = fixture.context.createRouterSocket()) {
            fixture.target.setCanonicalActorJoinHandler(
                    (peer, request) -> {
                        fixture.received.incrementAndGet();
                        targetEntered.complete(null);
                        return targetResponse;
                    });
            var join = fixture.join(Duration.ofSeconds(5));
            fixture.assertWaiting(join);
            fixture.publishDescriptor();
            targetEntered.get(3, TimeUnit.SECONDS);

            String endpoint = "inproc://connection-observer-" + UUID.randomUUID();
            listener.setRoutingId(RoutingId.from("new-peer"));
            listener.bind(endpoint);

            var initialSignal = new CompletableFuture<Void>();
            var observerEntered = new CompletableFuture<Long>();
            var releaseObserver = new CompletableFuture<Void>();
            var observerExited = new CompletableFuture<Long>();
            // A separate RID receives connection broadcasts but no targeted route signals.
            try (var observer =
                    fixture.source.onPeerStateChanged(
                            RoutingId.from("observer-only"),
                            () -> {
                                if (!initialSignal.complete(null)
                                        && observerEntered.complete(System.nanoTime())) {
                                    releaseObserver.join();
                                    observerExited.complete(System.nanoTime());
                                }
                            },
                            () -> {})) {
                try {
                    initialSignal.get(3, TimeUnit.SECONDS);
                    fixture.source.connectPeer(endpoint);
                    long entered = observerEntered.get(3, TimeUnit.SECONDS);
                    assertEquals(1, fixture.bindingRequests.get());
                    assertEquals(1, fixture.received.get());
                    assertEquals(1, fixture.source.operationRegistry().pendingCount());
                    assertTrue(
                            targetResponse.complete(
                                    new CanonicalActorJoinResponse(true, 7, List.of())));

                    try {
                        var reply = join.get(2, TimeUnit.SECONDS);
                        assertTrue(reply.accepted());
                        assertFalse(releaseObserver.isDone());
                        assertEquals(0, fixture.source.operationRegistry().pendingCount());
                        System.out.println(
                                "Actor Join accepted with observer blocked and pending=0");
                    } catch (TimeoutException timeout) {
                        throw new AssertionError(
                                "Actor Join did not complete while the observer was blocked,"
                                        + " pending="
                                        + fixture.source.operationRegistry().pendingCount(),
                                timeout);
                    } finally {
                        long remaining =
                                Duration.ofMillis(2100).toNanos() - (System.nanoTime() - entered);
                        if (remaining > 0) TimeUnit.NANOSECONDS.sleep(remaining);
                        releaseObserver.complete(null);
                        long held = observerExited.get(3, TimeUnit.SECONDS) - entered;
                        assertTrue(held > Duration.ofSeconds(2).toNanos());
                        System.out.printf(
                                "Connection observer held for %d ms%n",
                                TimeUnit.NANOSECONDS.toMillis(held));
                    }
                } finally {
                    releaseObserver.complete(null);
                    targetResponse.cancel(false);
                }
            }
        }
    }
}
