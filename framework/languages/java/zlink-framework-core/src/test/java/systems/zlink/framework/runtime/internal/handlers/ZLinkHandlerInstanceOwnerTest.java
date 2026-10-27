package systems.zlink.framework.runtime.internal.handlers;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkHandlerInstanceOwnerTest {
    @Test
    void concurrentCloseWaitsForTheOneActivationCleanup() throws Exception {
        var entered = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        var cleanupCalls = new AtomicInteger();
        var activator =
                new ZLinkHandlerActivator() {
                    @Override
                    public Object create(Class<?> type) {
                        return new TestHandler();
                    }

                    @Override
                    public Activation openActivation() {
                        return new Activation() {
                            @Override
                            public Object create(Class<?> type) {
                                return new TestHandler();
                            }

                            @Override
                            public void destroy(Object instance) {}

                            @Override
                            public void close() {
                                if (cleanupCalls.incrementAndGet() != 1) return;
                                entered.countDown();
                                try {
                                    assertTrue(release.await(3, TimeUnit.SECONDS));
                                } catch (InterruptedException failure) {
                                    Thread.currentThread().interrupt();
                                    throw new AssertionError(failure);
                                }
                            }
                        };
                    }
                };
        var owner = new ZLinkHandlerInstanceOwner(activator);
        owner.instance(TestHandler.class);
        try (var executor = Executors.newVirtualThreadPerTaskExecutor()) {
            var first = executor.submit(owner::close);
            try {
                assertTrue(entered.await(3, TimeUnit.SECONDS));
                var second = executor.submit(owner::close);
                assertThrows(
                        java.util.concurrent.TimeoutException.class,
                        () -> second.get(100, TimeUnit.MILLISECONDS));
                release.countDown();
                first.get(3, TimeUnit.SECONDS);
                second.get(3, TimeUnit.SECONDS);
                assertEquals(1, cleanupCalls.get());
            } finally {
                release.countDown();
            }
        }
    }

    @Test
    void unusedDispatchDoesNotOpenAnActivation() {
        AtomicInteger opened = new AtomicInteger();
        var activator =
                new ZLinkHandlerActivator() {
                    @Override
                    public Object create(Class<?> type) {
                        return new TestHandler();
                    }

                    @Override
                    public Activation openActivation() {
                        opened.incrementAndGet();
                        return ZLinkHandlerActivator.super.openActivation();
                    }
                };
        try (var owner = new ZLinkHandlerInstanceOwner(activator)) {
            owner.prepare(TestHandler.class);
            assertEquals(0, opened.get());
        }
        assertEquals(0, opened.get());
    }

    @Test
    void activationRunsOnTheCallingHandlerTurn() {
        Thread handlerThread = Thread.currentThread();
        AtomicReference<Thread> activationThread = new AtomicReference<>();
        try (var owner =
                new ZLinkHandlerInstanceOwner(
                        handlerType -> {
                            activationThread.set(Thread.currentThread());
                            return new TestHandler();
                        })) {
            owner.instance(TestHandler.class);
            assertSame(
                    handlerThread,
                    activationThread.get(),
                    "handler activation must not offload the existing handler turn");
        }
    }

    @Test
    void concurrentLookupsRetainSingleActivationAndCloseOwnership() throws Exception {
        AtomicInteger creates = new AtomicInteger();
        AtomicInteger destroys = new AtomicInteger();
        ZLinkHandlerActivator activator =
                new ZLinkHandlerActivator() {
                    @Override
                    public Object create(Class<?> handlerType) {
                        creates.incrementAndGet();
                        return new TestHandler();
                    }

                    @Override
                    public void destroy(Object instance) {
                        destroys.incrementAndGet();
                    }
                };
        try (var threads = Executors.newVirtualThreadPerTaskExecutor();
                var owner = new ZLinkHandlerInstanceOwner(activator)) {
            CountDownLatch ready = new CountDownLatch(16);
            CompletableFuture<Void> start = new CompletableFuture<>();
            List<CompletableFuture<Object>> lookups = new ArrayList<>();
            for (int index = 0; index < 16; index++) {
                lookups.add(
                        CompletableFuture.supplyAsync(
                                () -> {
                                    ready.countDown();
                                    start.join();
                                    return owner.instance(TestHandler.class);
                                },
                                threads));
            }
            try {
                assertTrue(ready.await(3, TimeUnit.SECONDS));
            } finally {
                start.complete(null);
            }
            Object instance = lookups.getFirst().get(3, TimeUnit.SECONDS);
            for (CompletableFuture<Object> lookup : lookups) {
                assertSame(instance, lookup.get(3, TimeUnit.SECONDS));
            }
            owner.close();
            owner.close();
            assertThrows(IllegalStateException.class, () -> owner.instance(TestHandler.class));
        }
        assertEquals(1, creates.get());
        assertEquals(1, destroys.get());
    }

    @Test
    void reusesHandlerInsideOneActivationAndRecreatesItForTheNextActivation() {
        AtomicInteger creates = new AtomicInteger();
        AtomicInteger destroys = new AtomicInteger();
        ZLinkHandlerActivator activator =
                new ZLinkHandlerActivator() {
                    @Override
                    public Object create(Class<?> handlerType) {
                        creates.incrementAndGet();
                        return new TestHandler();
                    }

                    @Override
                    public void destroy(Object instance) {
                        destroys.incrementAndGet();
                    }
                };

        TestHandler first;
        try (var activation = new ZLinkHandlerInstanceOwner(activator)) {
            first = (TestHandler) activation.instance(TestHandler.class);
            assertSame(first, activation.instance(TestHandler.class));
        }
        try (var activation = new ZLinkHandlerInstanceOwner(activator)) {
            assertNotSame(first, activation.instance(TestHandler.class));
            activation.close();
        }

        assertEquals(2, creates.get());
        assertEquals(2, destroys.get());
    }

    @ParameterizedTest
    @CsvSource({"false", "true"})
    void failedOwnerReleasesStayOwnedAndSuccessfulReleasesAreNotRepeated(
            boolean failFirstScopeClose) {
        var destroyFailure = new IllegalStateException("first destroy failed");
        var closeFailure = new IllegalStateException("first scope close failed");
        var destroyCalls = new AtomicInteger();
        var closeCalls = new AtomicInteger();
        var handler = new TestHandler();
        var activator =
                new ZLinkHandlerActivator() {
                    @Override
                    public Object create(Class<?> type) {
                        return handler;
                    }

                    @Override
                    public Activation openActivation() {
                        return new Activation() {
                            @Override
                            public Object create(Class<?> type) {
                                return handler;
                            }

                            @Override
                            public void destroy(Object instance) {
                                assertSame(handler, instance);
                                if (destroyCalls.incrementAndGet() == 1) throw destroyFailure;
                            }

                            @Override
                            public void close() {
                                if (closeCalls.incrementAndGet() == 1 && failFirstScopeClose)
                                    throw closeFailure;
                            }
                        };
                    }
                };
        var owner = new ZLinkHandlerInstanceOwner(activator);
        assertSame(handler, owner.instance(TestHandler.class));
        RuntimeException observed = assertThrows(RuntimeException.class, owner::close);
        assertSame(destroyFailure, observed);
        assertEquals(1, destroyCalls.get());
        assertEquals(1, closeCalls.get());
        assertEquals(failFirstScopeClose ? 1 : 0, destroyFailure.getSuppressed().length);
        if (failFirstScopeClose) assertSame(closeFailure, destroyFailure.getSuppressed()[0]);
        assertThrows(IllegalStateException.class, () -> owner.instance(TestHandler.class));

        owner.close();
        assertEquals(2, destroyCalls.get());
        assertEquals(failFirstScopeClose ? 2 : 1, closeCalls.get());
        owner.close();
        assertEquals(2, destroyCalls.get());
        assertEquals(failFirstScopeClose ? 2 : 1, closeCalls.get());
    }

    @Test
    void retryDoesNotDestroyOtherSuccessfullyReleasedInstancesAgain() {
        var first = new TestHandler();
        var second = new Object();
        var firstCalls = new AtomicInteger();
        var secondCalls = new AtomicInteger();
        var activator =
                new ZLinkHandlerActivator() {
                    @Override
                    public Object create(Class<?> type) {
                        return type == TestHandler.class ? first : second;
                    }

                    @Override
                    public void destroy(Object instance) {
                        if (instance == first && firstCalls.incrementAndGet() == 1)
                            throw new IllegalStateException("first destroy failed");
                        if (instance == second) secondCalls.incrementAndGet();
                    }
                };
        var owner = new ZLinkHandlerInstanceOwner(activator);
        owner.instance(TestHandler.class);
        owner.instance(Object.class);
        assertThrows(IllegalStateException.class, owner::close);
        owner.close();
        owner.close();
        assertEquals(2, firstCalls.get());
        assertEquals(1, secondCalls.get());
    }

    private static final class TestHandler {}
}
