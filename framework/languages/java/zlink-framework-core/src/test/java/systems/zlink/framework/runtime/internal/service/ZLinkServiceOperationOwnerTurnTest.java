package systems.zlink.framework.runtime.internal.service;

import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;


import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Tests the registration/submission boundary with a controlled binding submission. */
final class ZLinkServiceOperationOwnerTurnTest {
    @Test
    void closeCannotSplitRegistrationFromBindingSubmission() throws Exception {
        try (var registry = new ZLinkServiceOperationRegistry()) {
            var submissionEntered = new CountDownLatch(1);
            var closeReturned = new CountDownLatch(1);
            var interleaved = new AtomicBoolean();
            var submissions = new AtomicInteger();
            var closer = Thread.ofPlatform().start(() -> {
                try {
                    submissionEntered.await();
                    registry.close();
                    closeReturned.countDown();
                } catch (InterruptedException failure) {
                    Thread.currentThread().interrupt();
                    throw new AssertionError(failure);
                }
            });
            try {
                registry.submitTerminal(UUID.randomUUID(), Duration.ofSeconds(5), terminal -> {
                    submissionEntered.countDown();
                    try {
                        interleaved.set(closeReturned.await(1, TimeUnit.SECONDS));
                    } catch (InterruptedException failure) {
                        throw new AssertionError(failure);
                    }
                    submissions.incrementAndGet();
                    return CompletableFuture.completedFuture(null);
                }, ignored -> {});
            } finally {
                submissionEntered.countDown();
                closer.join(2000);
            }
            assertFalse(closer.isAlive());
            assertEquals(1, submissions.get());
            assertFalse(interleaved.get(), "close returned between E3 and E4");
        }
    }
    @Test
    void bindingCompletionTakesAuthorityWithoutWaitingForTheOwner() throws Exception {
        var lane = new systems.zlink.framework.runtime.internal.execution.ZLinkStateLane();
        try (var registry = new ZLinkServiceOperationRegistry(lane, new IllegalStateException("closed"))) {
            var operation = ZLinkCompletionBridge.await(registry.<String>register(Duration.ofSeconds(5)));
            var entered = new CountDownLatch(1);
            var release = new CountDownLatch(1);
            var occupied = lane.runAsync(() -> {
                entered.countDown();
                try { release.await(); }
                catch (InterruptedException failure) { throw new AssertionError(failure); }
            });
            try {
                assertTrue(entered.await(1, TimeUnit.SECONDS));
                assertTrue(registry.complete(operation.id(), "reply"));
                assertEquals(0, registry.pendingCount());
                assertEquals("reply", operation.get(1, TimeUnit.SECONDS));
            } finally {
                release.countDown();
                occupied.toCompletableFuture().get(1, TimeUnit.SECONDS);
            }
        }
    }

    @Test
    void staleDeadlineCannotRemoveANewEntryWithTheSameIdentity() {
        var clock = new AtomicLong();
        try (var registry = new ZLinkServiceOperationRegistry(
                command -> new Thread(), new IllegalStateException("closed"), clock::get)) {
            UUID id = UUID.randomUUID();
            var first = ZLinkCompletionBridge.await(registry.<String>register(id, Duration.ofMillis(1)));
            assertTrue(registry.complete(id, "first"));
            assertEquals("first", first.join());
            var second = ZLinkCompletionBridge.await(registry.<String>register(id, Duration.ofHours(1)));
            clock.set(TimeUnit.MILLISECONDS.toNanos(2));
            assertEquals(0, ZLinkCompletionBridge.await(registry.expire(clock.get())));
            assertEquals(1, registry.pendingCount());
            assertFalse(second.isDone());
            assertTrue(registry.complete(id, "second"));
            assertEquals("second", second.join());
        }
    }

}
