package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.calls.ZLinkBlockingCalls;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkServiceCompletionTurnTest {
    @Test
    void nestedCompletionRestoresItsScopeAfterFailure() {
        var dispatcher = ZLinkServiceCompletionDispatcher.INSTANCE;
        var beforeNested = new AtomicReference<Boolean>();
        var duringNested = new AtomicReference<Boolean>();
        var afterNested = new AtomicReference<Boolean>();
        var blockingFailure = new AtomicReference<ZLinkFrameworkException>();
        var nested =
                new ZLinkServiceCompletionDispatcher.WorkItem() {
                    void dispatch() {
                        duringNested.set(ZLinkServiceCompletionDispatcher.isCurrentCompletion());
                        throw new IllegalStateException("nested completion failed");
                    }
                };
        var outer =
                new ZLinkServiceCompletionDispatcher.WorkItem() {
                    void dispatch() {
                        beforeNested.set(ZLinkServiceCompletionDispatcher.isCurrentCompletion());
                        nested.run();
                        afterNested.set(ZLinkServiceCompletionDispatcher.isCurrentCompletion());
                        try {
                            ZLinkBlockingCalls.requireMayBlock();
                        } catch (ZLinkFrameworkException failure) {
                            blockingFailure.set(failure);
                        }
                    }
                };
        dispatcher.register(nested);
        dispatcher.register(outer);
        outer.run();
        assertEquals(Boolean.TRUE, beforeNested.get());
        assertEquals(Boolean.TRUE, duringNested.get());
        assertEquals(Boolean.TRUE, afterNested.get());
        assertNull(
                blockingFailure.get(), "completion delivery does not own a runtime execution gate");
        assertFalse(ZLinkServiceCompletionDispatcher.isCurrentCompletion());
        assertDoesNotThrow(ZLinkBlockingCalls::requireMayBlock);
    }

    @Test
    void dedicatedInfrastructureStillRejectsBlocking() throws Exception {
        var rejection = new AtomicReference<ZLinkFrameworkException>();
        var infrastructure =
                new Thread(
                        ZLinkBlockingCalls.infrastructureTask(
                                () -> {
                                    try {
                                        ZLinkBlockingCalls.requireMayBlock();
                                    } catch (ZLinkFrameworkException failure) {
                                        rejection.set(failure);
                                    }
                                }),
                        "blocking-guard-infrastructure");
        infrastructure.start();
        infrastructure.join();
        assertNotNull(rejection.get());
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.INVALID_OPERATION,
                rejection.get().kind());
        assertDoesNotThrow(ZLinkBlockingCalls::requireMayBlock);
    }

    @Test
    void nestedCallerViewsReuseTheFirstDeliveryTurn() throws Exception {
        var source = new CompletableFuture<String>();
        var inner = ZLinkSerialExecutionQueue.manageCurrent(source);
        var outer = ZLinkSerialExecutionQueue.manageCurrent(inner);
        var observed = new CountDownLatch(1);
        var completeFrames = new AtomicLong();
        var callback =
                outer.thenRun(
                        () -> {
                            completeFrames.set(
                                    StackWalker.getInstance()
                                            .walk(
                                                    frames ->
                                                            frames.filter(
                                                                            frame ->
                                                                                    frame.getClassName()
                                                                                                    .equals(
                                                                                                            CompletableFuture
                                                                                                                    .class
                                                                                                                    .getName())
                                                                                            && frame.getMethodName()
                                                                                                    .equals(
                                                                                                            "complete"))
                                                                    .count()));
                            observed.countDown();
                        });
        source.complete("reply");
        assertTrue(observed.await(5, TimeUnit.SECONDS));
        callback.toCompletableFuture().join();
        assertEquals(
                2, completeFrames.get(), "both caller futures complete in the same delivery stack");
    }

    @Test
    void mappedServiceResultReusesTheRegisteredCompletionTurn() throws Exception {
        try (var registry = new ZLinkServiceOperationRegistry()) {
            var operation = registry.<String>register(Duration.ofHours(1));
            var mapped = operation.thenApply(value -> "mapped " + value);
            var caller = ZLinkSerialExecutionQueue.manageCurrent(mapped);
            var observed = new CountDownLatch(1);
            var onRegisteredTurn = new AtomicReference<Boolean>();
            var callback =
                    caller.thenRun(
                            () -> {
                                onRegisteredTurn.set(
                                        StackWalker.getInstance()
                                                .walk(
                                                        frames ->
                                                                frames.anyMatch(
                                                                        frame ->
                                                                                frame.getClassName()
                                                                                                .equals(
                                                                                                        ZLinkServiceOperationRegistry
                                                                                                                        .class
                                                                                                                        .getName()
                                                                                                                + "$Entry")
                                                                                        && frame.getMethodName()
                                                                                                .equals(
                                                                                                        "dispatch"))));
                                observed.countDown();
                            });
            assertTrue(registry.complete(operation.id(), "reply"));
            assertTrue(observed.await(5, TimeUnit.SECONDS));
            callback.toCompletableFuture().join();
            assertTrue(onRegisteredTurn.get(), "mapping adds no second completion publication");
            assertEquals("mapped reply", caller.toCompletableFuture().join());
        }
    }

    @Test
    void unrelatedServiceCompletionDoesNotWaitForTheFirstCallbackToReturn() throws Exception {
        var release = new CompletableFuture<Void>();
        var entered = new CountDownLatch(1);
        var observed = new CountDownLatch(1);
        try (var registry = new ZLinkServiceOperationRegistry()) {
            var first = registry.<String>register(Duration.ofHours(1));
            var second = registry.<String>register(Duration.ofHours(1));
            var firstCallback =
                    first.thenRun(
                            () -> {
                                entered.countDown();
                                release.join();
                            });
            var secondCallback = second.thenRun(observed::countDown);
            try {
                assertTrue(registry.complete(first.id(), "first"));
                assertTrue(entered.await(5, TimeUnit.SECONDS));
                assertTrue(registry.complete(second.id(), "second"));
                assertTrue(observed.await(5, TimeUnit.SECONDS));
            } finally {
                release.complete(null);
            }
            firstCallback.join();
            secondCallback.join();
        }
    }
}
