package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.function.BiConsumer;

/** Tests the registration/submission boundary with a controlled binding submission. */
final class ZLinkServiceOperationOwnerTurnTest {
    @Test
    void closeCannotSplitRegistrationFromBindingSubmission() throws Exception {
        try (var registry = new ZLinkServiceOperationRegistry()) {
            var submissionEntered = new CountDownLatch(1);
            var closeReturned = new CountDownLatch(1);
            var interleaved = new AtomicBoolean();
            var submissions = new AtomicInteger();
            var closer =
                    Thread.ofPlatform()
                            .start(
                                    () -> {
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
                registry.submitTerminal(
                        UUID.randomUUID(),
                        Duration.ofSeconds(5),
                        terminal -> {
                            submissionEntered.countDown();
                            try {
                                interleaved.set(closeReturned.await(1, TimeUnit.SECONDS));
                            } catch (InterruptedException failure) {
                                throw new AssertionError(failure);
                            }
                            submissions.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        },
                        ignored -> {});
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
        try (var registry =
                new ZLinkServiceOperationRegistry(lane, new IllegalStateException("closed"))) {
            var operation =
                    ZLinkCompletionBridge.await(registry.<String>register(Duration.ofSeconds(5)));
            var entered = new CountDownLatch(1);
            var release = new CountDownLatch(1);
            var occupied =
                    lane.runAsync(
                            () -> {
                                entered.countDown();
                                try {
                                    release.await();
                                } catch (InterruptedException failure) {
                                    throw new AssertionError(failure);
                                }
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
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        command -> new Thread(), new IllegalStateException("closed"), clock::get)) {
            UUID id = UUID.randomUUID();
            var first =
                    ZLinkCompletionBridge.await(
                            registry.<String>register(id, Duration.ofMillis(1)));
            assertTrue(registry.complete(id, "first"));
            assertEquals("first", first.join());
            var second =
                    ZLinkCompletionBridge.await(registry.<String>register(id, Duration.ofHours(1)));
            clock.set(TimeUnit.MILLISECONDS.toNanos(2));
            assertEquals(0, ZLinkCompletionBridge.await(registry.expire(clock.get())));
            assertEquals(1, registry.pendingCount());
            assertFalse(second.isDone());
            assertTrue(registry.complete(id, "second"));
            assertEquals("second", second.join());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void cancelledEntryTerminalCannotCompleteAReusedIdentity(boolean failure) throws Exception {
        try (var registry = new ZLinkServiceOperationRegistry()) {
            UUID id = UUID.randomUUID();
            var terminal = new AtomicReference<BiConsumer<String, Throwable>>();
            var discarded = new AtomicInteger();
            var old =
                    registry.<String>submitTerminal(
                            id,
                            Duration.ofSeconds(30),
                            callback -> {
                                terminal.set(callback);
                                return null;
                            },
                            value -> {
                                assertEquals("old reply", value);
                                discarded.incrementAndGet();
                            });
            assertNotNull(terminal.get());
            assertTrue(old.cancel(false));
            assertThrows(CancellationException.class, () -> old.get(1, TimeUnit.SECONDS));
            assertTrue(old.isCancelled());
            assertEquals(0, registry.pendingCount());
            var current =
                    ZLinkCompletionBridge.await(
                            registry.<String>register(id, Duration.ofSeconds(30)));
            terminal.get()
                    .accept(
                            failure ? null : "old reply",
                            failure ? new IllegalStateException("old failure") : null);
            assertEquals(1, registry.pendingCount());
            assertFalse(current.isDone());
            assertEquals(failure ? 0 : 1, discarded.get());
            assertTrue(registry.complete(id, "current reply"));
            assertEquals("current reply", current.get(1, TimeUnit.SECONDS));
            assertEquals(failure ? 0 : 1, discarded.get());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void expiredEntryTerminalCannotCompleteAReusedIdentity(boolean failure) throws Exception {
        var clock = new AtomicLong();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        command -> new Thread(), new IllegalStateException("closed"), clock::get)) {
            UUID id = UUID.randomUUID();
            var terminal = new AtomicReference<BiConsumer<String, Throwable>>();
            var discarded = new AtomicInteger();
            var old =
                    registry.<String>submitTerminal(
                            id,
                            Duration.ofMillis(1),
                            callback -> {
                                terminal.set(callback);
                                return null;
                            },
                            value -> {
                                assertEquals("old reply", value);
                                discarded.incrementAndGet();
                            });
            clock.set(TimeUnit.MILLISECONDS.toNanos(2));
            assertEquals(1, ZLinkCompletionBridge.await(registry.expire(clock.get())));
            assertInstanceOf(
                    TimeoutException.class,
                    assertThrows(ExecutionException.class, () -> old.get(1, TimeUnit.SECONDS))
                            .getCause());
            var current =
                    ZLinkCompletionBridge.await(registry.<String>register(id, Duration.ofHours(1)));
            terminal.get()
                    .accept(
                            failure ? null : "old reply",
                            failure ? new IllegalStateException("old failure") : null);
            assertEquals(1, registry.pendingCount());
            assertFalse(current.isDone());
            assertEquals(failure ? 0 : 1, discarded.get());
            assertTrue(registry.complete(id, "current reply"));
            assertEquals("current reply", current.get(1, TimeUnit.SECONDS));
            assertEquals(failure ? 0 : 1, discarded.get());
        }
    }

    @Test
    void discardedEntrySubmissionThrowCannotCompleteAReusedIdentity() throws Exception {
        try (var registry = new ZLinkServiceOperationRegistry()) {
            UUID id = UUID.randomUUID();
            var current = new AtomicReference<ZLinkServiceOperationRegistry.Operation<String>>();
            var old =
                    registry.<String>submitTerminal(
                            id,
                            Duration.ofSeconds(30),
                            terminal -> {
                                assertTrue(registry.discard(id));
                                current.set(
                                        ZLinkCompletionBridge.await(
                                                registry.<String>register(
                                                        id, Duration.ofSeconds(30))));
                                throw new IllegalStateException("old submission failure");
                            },
                            ignored -> fail("submission produced no value"));
            assertFalse(old.isDone());
            assertFalse(old.cancel(false));
            assertEquals(1, registry.pendingCount());
            assertFalse(current.get().isDone());
            var expected = new IllegalStateException("current failure");
            assertTrue(registry.completeExceptionally(id, expected));
            assertSame(
                    expected,
                    assertThrows(
                                    ExecutionException.class,
                                    () -> current.get().get(1, TimeUnit.SECONDS))
                            .getCause());
        }
    }
}
