package systems.zlink.framework.runtime.internal.handlers;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;

final class ZLinkHandlerInstanceOwnerTest {
    @Test
    void activationRunsOnTheCallingHandlerTurn() {
        Thread handlerThread = Thread.currentThread();
        AtomicReference<Thread> activationThread = new AtomicReference<>();
        try (var owner = new ZLinkHandlerInstanceOwner(handlerType -> {
            activationThread.set(Thread.currentThread());
            return new TestHandler();
        })) {
            owner.instance(TestHandler.class);
            assertSame(handlerThread, activationThread.get(),
                "handler activation must not offload the existing handler turn");
        }
    }

    @Test
    void concurrentLookupsRetainSingleActivationAndCloseOwnership() throws Exception {
        AtomicInteger creates = new AtomicInteger();
        AtomicInteger destroys = new AtomicInteger();
        ZLinkHandlerActivator activator = new ZLinkHandlerActivator() {
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
                lookups.add(CompletableFuture.supplyAsync(() -> {
                    ready.countDown();
                    start.join();
                    return owner.instance(TestHandler.class);
                }, threads));
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
            assertThrows(IllegalStateException.class,
                () -> owner.instance(TestHandler.class));
        }
        assertEquals(1, creates.get());
        assertEquals(1, destroys.get());
    }

    @Test
    void reusesHandlerInsideOneActivationAndRecreatesItForTheNextActivation() {
        AtomicInteger creates = new AtomicInteger();
        AtomicInteger destroys = new AtomicInteger();
        ZLinkHandlerActivator activator = new ZLinkHandlerActivator() {
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

    private static final class TestHandler {
    }
}
