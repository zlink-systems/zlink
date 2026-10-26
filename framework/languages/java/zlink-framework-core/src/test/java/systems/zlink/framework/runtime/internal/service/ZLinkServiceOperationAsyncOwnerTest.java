package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

final class ZLinkServiceOperationAsyncOwnerTest {
    @Test
    void foreignTurnReturnsTheRequestStageWhileItsOwnerIsBusy() throws Exception {
        var owner = new ZLinkStateLane();
        var foreign = new ZLinkStateLane();
        var entered = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        var attempts = new AtomicInteger();
        var binding = new CompletableFuture<String>();
        try (var registry = new ZLinkServiceOperationRegistry(owner, new IllegalStateException("closed"))) {
            var held = owner.runAsync(() -> {
                entered.countDown();
                try { release.await(); }
                catch (InterruptedException failure) { throw new AssertionError(failure); }
            });
            var returned = new CompletableFuture<CompletableFuture<String>>();
            try {
                assertTrue(entered.await(1, TimeUnit.SECONDS));
                foreign.runAsync(() -> returned.complete(registry.submit(
                        UUID.randomUUID(), Duration.ofSeconds(5), () -> {
                            assertTrue(owner.isOnLane());
                            assertEquals(1, registry.pendingCount());
                            attempts.incrementAndGet();
                            return binding;
                        }, ignored -> {})));
                var request = returned.get(1, TimeUnit.SECONDS);
                assertEquals(0, attempts.get());
                assertFalse(request.isDone());
                release.countDown();
                held.toCompletableFuture().get(1, TimeUnit.SECONDS);
                owner.runAsync(() -> {}).toCompletableFuture().get(1, TimeUnit.SECONDS);
                assertEquals(1, attempts.get());
                binding.complete("reply");
                assertEquals("reply", request.get(1, TimeUnit.SECONDS));
            } finally {
                release.countDown();
                held.toCompletableFuture().get(1, TimeUnit.SECONDS);
            }
        }
    }

    @Test
    void busyNodeDoesNotPreventAnotherNodesRegistrationAndSubmit() throws Exception {
        var ownerA = new ZLinkStateLane();
        var ownerB = new ZLinkStateLane();
        var entered = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        try (var nodeA = new ZLinkServiceOperationRegistry(ownerA, new IllegalStateException("A closed"));
                var nodeB = new ZLinkServiceOperationRegistry(ownerB, new IllegalStateException("B closed"))) {
            var held = ownerA.runAsync(() -> {
                entered.countDown();
                try { release.await(); }
                catch (InterruptedException failure) { throw new AssertionError(failure); }
            });
            try {
                assertTrue(entered.await(1, TimeUnit.SECONDS));
                var published = CompletableFuture.supplyAsync(() -> nodeA.submit(UUID.randomUUID(), Duration.ofSeconds(5),
                        () -> CompletableFuture.completedFuture("A"), ignored -> {}));
                var a = published.get(1, TimeUnit.SECONDS);
                var b = nodeB.submit(UUID.randomUUID(), Duration.ofSeconds(5), () -> {
                    assertTrue(ownerB.isOnLane());
                    assertEquals(1, nodeB.pendingCount());
                    return CompletableFuture.completedFuture("B");
                }, ignored -> {});
                assertEquals("B", b.get(1, TimeUnit.SECONDS));
                assertFalse(a.isDone());
                release.countDown();
                assertEquals("A", a.get(1, TimeUnit.SECONDS));
            } finally {
                release.countDown();
                held.toCompletableFuture().get(1, TimeUnit.SECONDS);
            }
        }
    }
    @Test
    void queuedCancellationDoesNotStartBindingOrLeaveAnEntry() throws Exception {
        var owner = new ZLinkStateLane();
        var entered = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        var attempts = new AtomicInteger();
        try (var registry = new ZLinkServiceOperationRegistry(owner, new IllegalStateException("closed"))) {
            var held = owner.runAsync(() -> {
                entered.countDown();
                try { release.await(); }
                catch (InterruptedException failure) { throw new AssertionError(failure); }
            });
            try {
                assertTrue(entered.await(1, TimeUnit.SECONDS));
                var caller = new CompletableFuture<String>();
                var registration = registry.submitTerminal(UUID.randomUUID(), Duration.ofSeconds(5),
                        terminal -> {
                            attempts.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        }, ignored -> {}, caller);
                assertTrue(caller.cancel(false));
                release.countDown();
                assertFalse(registration.toCompletableFuture().get(1, TimeUnit.SECONDS));
                assertEquals(0, attempts.get());
                assertEquals(0, registry.pendingCount());
            } finally {
                release.countDown();
                held.toCompletableFuture().get(1, TimeUnit.SECONDS);
            }
        }
    }

}
