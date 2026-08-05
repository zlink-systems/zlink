package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerTick;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;

final class ZLinkSpotTimerRegistryTest {
    @Test
    void emptyCanonicalTimerEnvelopeRestoresAsNoTimers() {
        var decoded = ZLinkSpotTimerRelocationEnvelope.decode(
            ZLinkSpotTimerRelocationEnvelope.encodeCanonical(List.of()),
            ignored -> TimerHandler.class);

        assertEquals(List.of(), decoded.timers());
    }

    @Test
    void reusesTimerHandlerForTheSpotActivation() throws Exception {
        ScheduledExecutorService executor = Executors.newSingleThreadScheduledExecutor();
        CountDownLatch handled = new CountDownLatch(2);
        AtomicInteger creates = new AtomicInteger();
        AtomicInteger destroys = new AtomicInteger();
        ZLinkHandlerActivator activator = new ZLinkHandlerActivator() {
            @Override
            public Object create(Class<?> handlerType) {
                creates.incrementAndGet();
                return new CountingTimerHandler(handled);
            }

            @Override
            public void destroy(Object instance) {
                destroys.incrementAndGet();
            }
        };
        ZLinkHandlerInstanceOwner handlers =
            new ZLinkHandlerInstanceOwner(activator);
        ZLinkSpotTimerRegistry registry = new ZLinkSpotTimerRegistry(
            "spot",
            executor,
            handlers,
            List.of(),
            null,
            "test",
            (timerName, operation) -> operation.get());
        registry.setSpot(new TestSpot());

        try {
            registry.add(
                "timer",
                Duration.ofMillis(1),
                CountingTimerHandler.class,
                null);
            assertTrue(handled.await(2, TimeUnit.SECONDS));
            assertEquals(1, creates.get());
        } finally {
            registry.close();
            handlers.close();
            executor.shutdownNow();
        }
        assertEquals(1, destroys.get());
    }

    @Test
    void dispatchesTimerHandlerThroughSpotExecutionPolicy() throws Exception {
        ScheduledExecutorService executor = Executors.newSingleThreadScheduledExecutor();
        CountDownLatch handled = new CountDownLatch(1);
        AtomicBoolean enteredDispatch = new AtomicBoolean();
        TimerHandler handler = new TimerHandler(handled, enteredDispatch);
        ZLinkSpotTimerRegistry registry = new ZLinkSpotTimerRegistry(
            "spot",
            executor,
            ignored -> handler,
            List.of(),
            null,
            "test",
            (timerName, operation) -> {
                enteredDispatch.set(true);
                return operation.get();
            });
        registry.setSpot(new TestSpot());

        try {
            registry.add("timer", Duration.ofMillis(1), TimerHandler.class, null);
            assertTrue(handled.await(2, TimeUnit.SECONDS));
            assertTrue(enteredDispatch.get());
        } finally {
            registry.close();
            executor.shutdownNow();
        }
    }

    @Test
    void replacingTimerNameSuppressesPreviousGeneration() throws Exception {
        ScheduledExecutorService executor = Executors.newSingleThreadScheduledExecutor();
        CountDownLatch replacementHandled = new CountDownLatch(1);
        AtomicBoolean previousHandled = new AtomicBoolean();
        ZLinkSpotTimerRegistry registry = new ZLinkSpotTimerRegistry(
            "spot",
            executor,
            type -> type == PreviousTimerHandler.class
                ? new PreviousTimerHandler(previousHandled)
                : new ReplacementTimerHandler(replacementHandled),
            List.of(),
            null,
            "test",
            (timerName, operation) -> operation.get());
        registry.setSpot(new TestSpot());

        try {
            registry.add(
                "timer",
                Duration.ofMillis(250),
                PreviousTimerHandler.class,
                null);
            registry.add(
                "timer",
                Duration.ofMillis(1),
                ReplacementTimerHandler.class,
                null);

            assertTrue(replacementHandled.await(2, TimeUnit.SECONDS));
            Thread.sleep(300);
            assertFalse(previousHandled.get());
        } finally {
            registry.close();
            executor.shutdownNow();
        }
    }

    @Test
    void relocationEnvelopeRestoresPendingTickWithoutRunningItAtSource()
        throws Exception {
        ScheduledExecutorService sourceExecutor =
            Executors.newSingleThreadScheduledExecutor();
        CountDownLatch pending = new CountDownLatch(1);
        AtomicReference<Supplier<CompletionStage<Void>>> queued =
            new AtomicReference<>();
        CompletableFuture<Void> queuedCompletion = new CompletableFuture<>();
        AtomicBoolean sourceHandled = new AtomicBoolean();
        ZLinkSpotTimerRegistry source = new ZLinkSpotTimerRegistry(
            "source",
            sourceExecutor,
            ignored -> new PreviousTimerHandler(sourceHandled),
            List.of(),
            null,
            "source",
            (timerName, operation) -> {
                queued.set(operation);
                pending.countDown();
                return queuedCompletion;
            });
        source.setSpot(new TestSpot());

        ZLinkSpotTimerRegistry.FrozenTimers frozen;
        byte[] encoded;
        try {
            source.add(
                "timer",
                Duration.ofMillis(1),
                PreviousTimerHandler.class,
                null);
            assertTrue(pending.await(2, TimeUnit.SECONDS));
            frozen = source.freeze();
            encoded = ZLinkSpotTimerRelocationEnvelope.encode(frozen);

            queued.get().get().whenComplete((ignored, failure) -> {
                if (failure == null) {
                    queuedCompletion.complete(null);
                } else {
                    queuedCompletion.completeExceptionally(failure);
                }
            });
            queuedCompletion.get(1, TimeUnit.SECONDS);
            assertFalse(sourceHandled.get());
        } finally {
            source.close();
            sourceExecutor.shutdownNow();
        }

        ScheduledExecutorService targetExecutor =
            Executors.newSingleThreadScheduledExecutor();
        CountDownLatch restored = new CountDownLatch(1);
        AtomicReference<ZLinkTimerTick> restoredTick = new AtomicReference<>();
        ZLinkSpotTimerRegistry target = new ZLinkSpotTimerRegistry(
            "target",
            targetExecutor,
            ignored -> new RestoredTimerHandler(restored, restoredTick),
            List.of(),
            null,
            "target",
            (timerName, operation) -> operation.get());
        target.setSpot(new TestSpot());
        try {
            var decoded = ZLinkSpotTimerRelocationEnvelope.decode(
                encoded,
                ignored -> RestoredTimerHandler.class);
            long expectedScheduledIndex = decoded.timers().getFirst()
                .pendingTick()
                .orElseThrow()
                .scheduledIndex();
            target.stageRestore(decoded);

            assertFalse(restored.await(100, TimeUnit.MILLISECONDS),
                "staged timers must not run before aggregate publication");
            target.publishStagedRestore();

            assertTrue(restored.await(2, TimeUnit.SECONDS));
            assertEquals(1, restoredTick.get().deliveryIndex());
            assertEquals(
                expectedScheduledIndex,
                restoredTick.get().scheduledIndex());
        } finally {
            target.close();
            targetExecutor.shutdownNow();
        }
    }

    @Test
    void relocationEnvelopeIsCanonicalForScheduledTimers() {
        ScheduledExecutorService executor =
            Executors.newSingleThreadScheduledExecutor();
        ZLinkSpotTimerRegistry registry = new ZLinkSpotTimerRegistry(
            "spot",
            executor,
            ignored -> new PreviousTimerHandler(new AtomicBoolean()),
            List.of(),
            null,
            "test",
            (timerName, operation) -> operation.get());
        registry.setSpot(new TestSpot());
        try {
            registry.add(
                "z-timer",
                Duration.ofHours(1),
                PreviousTimerHandler.class,
                null);
            registry.add(
                "a-timer",
                Duration.ofHours(1),
                PreviousTimerHandler.class,
                null);
            byte[] first = ZLinkSpotTimerRelocationEnvelope.encode(
                registry.freeze());
            byte[] second = ZLinkSpotTimerRelocationEnvelope.encode(
                ZLinkSpotTimerRelocationEnvelope.decode(
                    first,
                    ignored -> PreviousTimerHandler.class));

            assertArrayEquals(first, second);
        } finally {
            registry.close();
            executor.shutdownNow();
        }
    }

    @Test
    void relocationAbortResumesTheExistingTimerHandle() throws Exception {
        ScheduledExecutorService executor =
            Executors.newSingleThreadScheduledExecutor();
        CountDownLatch handled = new CountDownLatch(1);
        ZLinkSpotTimerRegistry registry = new ZLinkSpotTimerRegistry(
            "spot",
            executor,
            ignored -> new ReplacementTimerHandler(handled),
            List.of(),
            null,
            "test",
            (timerName, operation) -> operation.get());
        registry.setSpot(new TestSpot());
        try {
            ZLinkTimer timer = registry.add(
                    "timer",
                    Duration.ofMillis(100),
                    ReplacementTimerHandler.class,
                    null)
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            registry.freeze();
            Thread.sleep(150);
            assertEquals(1, handled.getCount());
            assertFalse(timer.isDisposed());

            registry.resume();
            assertTrue(handled.await(2, TimeUnit.SECONDS));
            assertFalse(timer.isDisposed());
        } finally {
            registry.close();
            executor.shutdownNow();
        }
    }

    @Test
    void relocationEnvelopeRejectsTrailingBytes() {
        assertThrows(
            systems.zlink.framework.errors.ZLinkConfigurationException.class,
            () -> ZLinkSpotTimerRelocationEnvelope.decode(
                new byte[] {0, 1, 2, 3},
                ignored -> PreviousTimerHandler.class));
    }

    public static final class TimerHandler {
        private final CountDownLatch handled;
        private final AtomicBoolean enteredDispatch;

        TimerHandler(CountDownLatch handled, AtomicBoolean enteredDispatch) {
            this.handled = handled;
            this.enteredDispatch = enteredDispatch;
        }

        public void handle(ZLinkSpot<?> spot, ZLinkTimerTick tick) {
            if (enteredDispatch.get()) {
                handled.countDown();
            }
        }
    }

    public static final class CountingTimerHandler {
        private final CountDownLatch handled;

        CountingTimerHandler(CountDownLatch handled) {
            this.handled = handled;
        }

        public void handle(ZLinkSpot<?> spot, ZLinkTimerTick tick) {
            handled.countDown();
        }
    }

    public static final class PreviousTimerHandler {
        private final AtomicBoolean handled;

        PreviousTimerHandler(AtomicBoolean handled) {
            this.handled = handled;
        }

        public void handle(ZLinkSpot<?> spot, ZLinkTimerTick tick) {
            handled.set(true);
        }
    }

    public static final class ReplacementTimerHandler {
        private final CountDownLatch handled;

        ReplacementTimerHandler(CountDownLatch handled) {
            this.handled = handled;
        }

        public void handle(ZLinkSpot<?> spot, ZLinkTimerTick tick) {
            handled.countDown();
        }
    }

    public static final class RestoredTimerHandler {
        private final CountDownLatch handled;
        private final AtomicReference<ZLinkTimerTick> tick;

        RestoredTimerHandler(
            CountDownLatch handled,
            AtomicReference<ZLinkTimerTick> tick) {
            this.handled = handled;
            this.tick = tick;
        }

        public void handle(ZLinkSpot<?> spot, ZLinkTimerTick value) {
            if (tick.compareAndSet(null, value)) {
                handled.countDown();
            }
        }
    }

    private static final class TestSpot implements ZLinkSpot<ZLinkActor> {
        @Override
        public ZLinkSpotContext context() {
            return null;
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
