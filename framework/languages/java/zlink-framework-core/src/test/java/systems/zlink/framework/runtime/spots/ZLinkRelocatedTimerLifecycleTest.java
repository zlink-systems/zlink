package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkTimer;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

final class ZLinkRelocatedTimerLifecycleTest {
    @Test
    void configureAndInitializeShareTheRestoredRegistrationAndCursor() throws Exception {
        withRuntime(
                (lifecycle, envelope) -> {
                    var prepared =
                            lifecycle
                                    .prepareRelocationReserved(
                                            "mesh", RebindingSpot.class, "relocated", 1, envelope)
                                    .toCompletableFuture()
                                    .join();
                    try {
                        var spot = (RebindingSpot) lifecycle.preparedSpot(prepared);
                        var context = prepared.created().activation().context;
                        assertNotNull(spot.configured);
                        assertNotNull(spot.configuredNew);
                        assertRestored(envelope, context);
                        lifecycle.completeRelocationReady(prepared).toCompletableFuture().join();
                        assertSame(spot.configured, spot.initialized);
                        assertRestored(envelope, context);
                        assertFalse(spot.configured.isDisposed());
                        assertNotNull(spot.newTimer);
                        lifecycle.publishReservedTimers(prepared);
                        var replacement =
                                spot.context()
                                        .addTimer(
                                                "restored",
                                                Duration.ofDays(1),
                                                ZLinkRelocatedTimerRebindTest.TickHandler.class,
                                                null)
                                        .toCompletableFuture()
                                        .join();
                        assertNotSame(spot.configured, replacement);
                        var replaced =
                                ZLinkSpotTimerRelocationEnvelope.canonicalize(
                                                context.freezeTimerRelocationEnvelope())
                                        .stream()
                                        .filter(timer -> timer.name().equals("restored"))
                                        .findFirst()
                                        .orElseThrow();
                        assertEquals(0, replaced.lastCompletedDeliveryIndex());
                        assertEquals(0, replaced.lastCompletedScheduledIndex());
                        assertTrue(spot.configured.isDisposed());
                    } finally {
                        lifecycle.discardReserved(prepared).toCompletableFuture().join();
                    }
                });
    }

    @Test
    void configureMismatchFailsActivation() throws Exception {
        withRuntime(
                (lifecycle, envelope) -> {
                    var failure =
                            assertThrows(
                                    CompletionException.class,
                                    () ->
                                            lifecycle
                                                    .prepareRelocationReserved(
                                                            "mesh",
                                                            ConfigureMismatchSpot.class,
                                                            "bad-configure",
                                                            1,
                                                            envelope)
                                                    .toCompletableFuture()
                                                    .join());
                    assertEquals(
                            ZLinkFrameworkErrorKind.INVALID_OPERATION,
                            assertInstanceOf(ZLinkFrameworkException.class, rootCause(failure))
                                    .kind());
                });
    }

    @Test
    void initializeMismatchFailsItsLifecycleCallback() throws Exception {
        withRuntime(
                (lifecycle, envelope) -> {
                    var prepared =
                            lifecycle
                                    .prepareRelocationReserved(
                                            "mesh",
                                            InitializeMismatchSpot.class,
                                            "bad-initialize",
                                            1,
                                            envelope)
                                    .toCompletableFuture()
                                    .join();
                    try {
                        var failure =
                                assertThrows(
                                        CompletionException.class,
                                        () ->
                                                lifecycle
                                                        .completeRelocationReady(prepared)
                                                        .toCompletableFuture()
                                                        .join());
                        assertEquals(
                                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                                assertInstanceOf(ZLinkFrameworkException.class, rootCause(failure))
                                        .kind());
                    } finally {
                        lifecycle.discardReserved(prepared).toCompletableFuture().join();
                    }
                });
    }

    private static void assertRestored(byte[] envelope, DefaultSpotContext context) {
        var actual =
                ZLinkSpotTimerRelocationEnvelope.canonicalize(
                        context.freezeTimerRelocationEnvelope());
        assertEquals(
                ZLinkSpotTimerRelocationEnvelope.canonicalize(envelope).getFirst(),
                actual.stream()
                        .filter(timer -> timer.name().equals("restored"))
                        .findFirst()
                        .orElseThrow());
    }

    private static Throwable rootCause(Throwable failure) {
        while (failure.getCause() != null) failure = failure.getCause();
        return failure;
    }

    private static void withRuntime(Scenario scenario) throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var server =
                options.addRouteMesh("mesh")
                        .listen("inproc://timer-rebind-" + UUID.randomUUID())
                        .objects()
                        .server();
        server.addSpotFactory(
                "rebind", RebindingSpot.class, factory -> factory.disableRelocation());
        server.addSpotFactory(
                "bad-configure",
                ConfigureMismatchSpot.class,
                factory -> factory.disableRelocation());
        server.addSpotFactory(
                "bad-initialize",
                InitializeMismatchSpot.class,
                factory -> factory.disableRelocation());
        try (var framework = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var host = (ZLinkSpotRuntime) framework.spotManager();
            var field = ZLinkSpotRuntime.class.getDeclaredField("spotLifecycle");
            field.setAccessible(true);
            scenario.run(
                    (ZLinkSpotLifecycle) field.get(host),
                    ZLinkSpotTimerRelocationEnvelope.encode(
                            ZLinkRelocatedTimerRebindTest.snapshot(false)));
        }
    }

    private interface Scenario {
        void run(ZLinkSpotLifecycle lifecycle, byte[] envelope);
    }

    public static class RebindingSpot implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;
        ZLinkTimer configured;
        ZLinkTimer configuredNew;
        ZLinkTimer initialized;
        ZLinkTimer newTimer;

        public RebindingSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            configured =
                    context.addTimer(
                                    "restored",
                                    Duration.ofDays(2),
                                    ZLinkRelocatedTimerRebindTest.TickHandler.class,
                                    null)
                            .toCompletableFuture()
                            .join();
            configuredNew =
                    context.addTimer(
                                    "configured-new",
                                    Duration.ofDays(1),
                                    ZLinkRelocatedTimerRebindTest.TickHandler.class,
                                    null)
                            .toCompletableFuture()
                            .join();
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            return context.addTimer(
                            "restored",
                            Duration.ofDays(3),
                            ZLinkRelocatedTimerRebindTest.TickHandler.class,
                            null)
                    .thenCompose(
                            timer -> {
                                initialized = timer;
                                return context.addTimer(
                                        "new",
                                        Duration.ofDays(1),
                                        ZLinkRelocatedTimerRebindTest.TickHandler.class,
                                        null);
                            })
                    .thenAccept(timer -> newTimer = timer);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ConfigureMismatchSpot extends RebindingSpot {
        public ConfigureMismatchSpot(ZLinkSpotContext context) {
            super(context);
        }

        @Override
        public void configure() {
            context()
                    .addTimer(
                            "restored",
                            Duration.ofDays(2),
                            ZLinkRelocatedTimerRebindTest.OtherHandler.class,
                            null)
                    .toCompletableFuture()
                    .join();
        }
    }

    public static final class InitializeMismatchSpot extends RebindingSpot {
        public InitializeMismatchSpot(ZLinkSpotContext context) {
            super(context);
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            return context()
                    .addTimer(
                            "restored",
                            Duration.ofDays(2),
                            ZLinkRelocatedTimerRebindTest.OtherHandler.class,
                            null)
                    .thenApply(ignored -> null);
        }
    }
}
