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
    void constructorTimerDoesNotRunBeforeFactoryReturnsAndReadyPublication() throws Exception {
        constructorTimerStaysBehindPreparation(false);
    }

    @Test
    void targetConstructorTimerDoesNotRunBeforeFactoryReturns() throws Exception {
        constructorTimerStaysBehindPreparation(true);
    }

    private void constructorTimerStaysBehindPreparation(boolean target) throws Exception {
        ConstructorTimerSpot.entered = new CompletableFuture<>();
        ConstructorTimerSpot.release = new CompletableFuture<>();
        ConstructorTimerSpot.deliveries.set(0);
        ConstructorTimerInstance.ticked = null;
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh("mesh")
                .listen("inproc://constructor-timer-" + UUID.randomUUID())
                .objects()
                .server()
                .addSpotFactory(
                        "constructor-timer",
                        ConstructorTimerSpot.class,
                        factory -> factory.disableRelocation());
        try (var controlled = new ControlledTimerRuntime(options);
                var creator = java.util.concurrent.Executors.newSingleThreadExecutor()) {
            var lifecycle = controlled.lifecycle;
            var preparing =
                    CompletableFuture.supplyAsync(
                            () ->
                                    (target
                                                    ? lifecycle.prepareRelocationReserved(
                                                            "mesh",
                                                            ConstructorTimerSpot.class,
                                                            "constructor-timer",
                                                            1,
                                                            ZLinkSpotTimerRelocationEnvelope.encode(
                                                                    new ZLinkSpotTimerRegistry
                                                                            .FrozenTimers(
                                                                            java.util.List.of())))
                                                    : lifecycle.prepareReserved(
                                                            "mesh",
                                                            ConstructorTimerSpot.class,
                                                            "constructor-timer",
                                                            1,
                                                            null))
                                            .toCompletableFuture()
                                            .join(),
                            creator);
            try {
                var context =
                        ConstructorTimerSpot.entered.get(3, java.util.concurrent.TimeUnit.SECONDS);
                controlled.controlTimerLane(context);
                var due = controlled.scheduler.due.poll(3, java.util.concurrent.TimeUnit.SECONDS);
                assertNotNull(due);
                due.run();
                assertFalse(
                        preparing.isDone(), "the factory must still be held inside construction");
                assertEquals(
                        0,
                        ConstructorTimerSpot.deliveries.get(),
                        "actual timer callback started before the factory returned");
                ConstructorTimerSpot.release.complete(null);
                var prepared = preparing.get(3, java.util.concurrent.TimeUnit.SECONDS);
                assertEquals(0, ConstructorTimerSpot.deliveries.get());
                var hold =
                        target
                                ? lifecycle
                                        .beginReservedIngressHold(prepared)
                                        .toCompletableFuture()
                                        .get(3, java.util.concurrent.TimeUnit.SECONDS)
                                : null;
                if (target)
                    lifecycle
                            .completeRelocationReady(prepared)
                            .toCompletableFuture()
                            .get(3, java.util.concurrent.TimeUnit.SECONDS);
                lifecycle
                        .publishReserved(prepared)
                        .toCompletableFuture()
                        .get(3, java.util.concurrent.TimeUnit.SECONDS);
                if (target) {
                    // Restoring the target envelope replaces constructor registrations.
                    lifecycle.publishReservedTimers(prepared);
                    lifecycle
                            .resumeReservedIngress(prepared, hold)
                            .toCompletableFuture()
                            .get(3, java.util.concurrent.TimeUnit.SECONDS);
                    assertEquals(0, ConstructorTimerSpot.deliveries.get());
                } else assertEquals(1, ConstructorTimerSpot.deliveries.get());
            } finally {
                ConstructorTimerSpot.release.complete(null);
                var prepared =
                        preparing
                                .handle((value, failure) -> value)
                                .get(3, java.util.concurrent.TimeUnit.SECONDS);
                if (prepared != null) {
                    if (lifecycle.spotActivationFor(prepared.spotId())
                            == prepared.created().activation())
                        lifecycle.retireClosed(prepared.created().activation());
                    lifecycle
                            .discardReserved(prepared)
                            .toCompletableFuture()
                            .get(3, java.util.concurrent.TimeUnit.SECONDS);
                }
            }
        }
    }

    @Test
    void instanceConstructorTimerDoesNotRunBeforeFactoryReturnsAndRecoveryCompletes()
            throws Exception {
        ConstructorTimerSpot.entered = new CompletableFuture<>();
        ConstructorTimerSpot.release = new CompletableFuture<>();
        ConstructorTimerSpot.deliveries.set(0);
        ConstructorTimerInstance.ticked = new CompletableFuture<>();
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRelocationStore(new systems.zlink.framework.runtime.InMemoryRelocationStore());
        options.addRouteMesh("mesh")
                .listen("inproc://instance-constructor-" + UUID.randomUUID())
                .objects()
                .server()
                .addInstanceSpotFactory(
                        "constructor-instance",
                        ConstructorTimerInstance.class,
                        factory -> factory.disableRelocation());
        try (var controlled = new ControlledTimerRuntime(options)) {
            var first =
                    controlled
                            .framework
                            .route()
                            .requestToSpot(
                                    "constructor-instance-" + UUID.randomUUID(),
                                    new InstanceProbe())
                            .instanceSpot("constructor-instance")
                            .inMesh("mesh")
                            .timeout(Duration.ofSeconds(5))
                            .submit(InstanceReply.class)
                            .toCompletableFuture();
            try {
                var context =
                        ConstructorTimerSpot.entered.get(3, java.util.concurrent.TimeUnit.SECONDS);
                controlled.controlTimerLane(context);
                var due = controlled.scheduler.due.poll(3, java.util.concurrent.TimeUnit.SECONDS);
                assertNotNull(due);
                due.run();
                assertFalse(first.isDone(), "the constructor must still be held");
                assertEquals(
                        0,
                        ConstructorTimerSpot.deliveries.get(),
                        "actual Instance timer callback started before the factory returned");
                ConstructorTimerSpot.release.complete(null);
                assertEquals(
                        new InstanceReply("constructor"),
                        first.get(3, java.util.concurrent.TimeUnit.SECONDS));
                ConstructorTimerInstance.ticked.get(3, java.util.concurrent.TimeUnit.SECONDS);
                assertEquals(1, ConstructorTimerSpot.deliveries.get());
            } finally {
                ConstructorTimerSpot.release.complete(null);
                first.handle((value, failure) -> null)
                        .get(3, java.util.concurrent.TimeUnit.SECONDS);
            }
        }
    }

    public static final class ConstructorTimerInstance
            implements systems.zlink.framework.spots.ZLinkInstanceSpot {
        static CompletableFuture<Void> ticked;
        final systems.zlink.framework.spots.ZLinkInstanceSpotContext context;

        public ConstructorTimerInstance(
                systems.zlink.framework.spots.ZLinkInstanceSpotContext context) {
            this.context = context;
            context.addTimer(
                            "constructor-ready",
                            Duration.ofNanos(1),
                            ConstructorTimerHandler.class,
                            null)
                    .toCompletableFuture()
                    .join();
            ConstructorTimerSpot.entered.complete(context);
            ConstructorTimerSpot.release.join();
        }

        @Override
        public systems.zlink.framework.spots.ZLinkInstanceSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addPacket(ConstructorInstanceHandler.class);
        }
    }

    public static final class ConstructorInstanceHandler
            implements systems.zlink.framework.spots.ZLinkSpotRequestHandler<
                    ConstructorTimerInstance, InstanceProbe, InstanceReply> {
        public CompletionStage<InstanceReply> handle(
                ConstructorTimerInstance spot, InstanceProbe request) {
            return CompletableFuture.completedFuture(new InstanceReply("constructor"));
        }
    }

    public static final class ConstructorTimerSpot extends ReadyTimerSpot {
        static CompletableFuture<Object> entered;
        static CompletableFuture<Void> release;
        static final java.util.concurrent.atomic.AtomicInteger deliveries =
                new java.util.concurrent.atomic.AtomicInteger();

        public ConstructorTimerSpot(ZLinkSpotContext context) {
            super(context);
            context.addTimer(
                            "constructor-ready",
                            Duration.ofNanos(1),
                            ConstructorTimerHandler.class,
                            null)
                    .toCompletableFuture()
                    .join();
            entered.complete(context);
            release.join();
        }

        @Override
        public CompletionStage<systems.zlink.framework.spots.ZLinkSpotCreateResponse> onCreate(
                systems.zlink.framework.messaging.ZLinkMessage request) {
            return CompletableFuture.completedFuture(
                    systems.zlink.framework.spots.ZLinkSpotCreateResponse.accept());
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ConstructorTimerHandler {
        public CompletionStage<Void> handle(
                Object spot, systems.zlink.framework.spots.ZLinkTimerTick tick) {
            ConstructorTimerSpot.deliveries.incrementAndGet();
            if (ConstructorTimerInstance.ticked != null)
                ConstructorTimerInstance.ticked.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }

    @Test
    void relocationTargetKeepsPendingTickAndCloseBehindItsExistingStagingSeal() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh("mesh")
                .listen("inproc://target-ready-" + UUID.randomUUID())
                .objects()
                .server()
                .addSpotFactory(
                        "target-timer",
                        TargetTimerSpot.class,
                        factory -> factory.disableRelocation());
        var old = ZLinkRelocatedTimerRebindTest.snapshot(true).timers().getFirst();
        var envelope =
                ZLinkSpotTimerRelocationEnvelope.encode(
                        new ZLinkSpotTimerRegistry.FrozenTimers(
                                java.util.List.of(
                                        new ZLinkSpotTimerRegistry.TimerSnapshot(
                                                old.name(),
                                                ReadyTimerHandler.class,
                                                old.schedule(),
                                                old.nextScheduledAt(),
                                                old.pendingTick()))));
        try (var controlled = new ControlledTimerRuntime(options)) {
            var lifecycle = controlled.lifecycle;
            var prepared =
                    lifecycle
                            .prepareRelocationReserved(
                                    "mesh", TargetTimerSpot.class, "target-timer", 1, envelope)
                            .toCompletableFuture()
                            .get(3, java.util.concurrent.TimeUnit.SECONDS);
            boolean published = false;
            try {
                var spot = (TargetTimerSpot) lifecycle.preparedSpot(prepared);
                controlled.controlTimerLane(spot.context());
                lifecycle
                        .completeRelocationReady(prepared)
                        .toCompletableFuture()
                        .get(3, java.util.concurrent.TimeUnit.SECONDS);
                assertEquals(0, spot.ticks);
                assertFalse(spot.closeRequested.toCompletableFuture().isDone());
                var hold =
                        lifecycle
                                .beginReservedIngressHold(prepared)
                                .toCompletableFuture()
                                .get(3, java.util.concurrent.TimeUnit.SECONDS);
                lifecycle
                        .publishReserved(prepared)
                        .toCompletableFuture()
                        .get(3, java.util.concurrent.TimeUnit.SECONDS);
                published = true;
                lifecycle.publishReservedTimers(prepared);
                assertEquals(
                        0,
                        spot.ticks,
                        "timer publication must retain the existing target staging seal");
                assertFalse(spot.closeRequested.toCompletableFuture().isDone());
                lifecycle
                        .resumeReservedIngress(prepared, hold)
                        .toCompletableFuture()
                        .get(3, java.util.concurrent.TimeUnit.SECONDS);
                assertFalse(
                        spot.closeRequested
                                .toCompletableFuture()
                                .get(3, java.util.concurrent.TimeUnit.SECONDS));
                assertEquals(
                        1,
                        spot.ticks,
                        "the pending actual callback must run after staging publication completes");
            } finally {
                if (published) lifecycle.retireClosed(prepared.created().activation());
                lifecycle
                        .discardReserved(prepared)
                        .toCompletableFuture()
                        .get(3, java.util.concurrent.TimeUnit.SECONDS);
            }
        }
    }

    public static final class TargetTimerSpot extends ReadyTimerSpot {
        public TargetTimerSpot(ZLinkSpotContext context) {
            super(context);
        }

        @Override
        public void configure() {
            context()
                    .addTimer("restored", Duration.ofDays(2), ReadyTimerHandler.class, null)
                    .toCompletableFuture()
                    .join();
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            closeRequested = context().close();
            return CompletableFuture.completedFuture(null);
        }
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.CsvSource({
        "SPOT_WIDE,READY,false", "PER_ACTOR,READY,false",
        "SPOT_WIDE,READY,true", "PER_ACTOR,READY,true",
        "SPOT_WIDE,REJECTED,true", "PER_ACTOR,REJECTED,true",
        "SPOT_WIDE,FAILED,true", "PER_ACTOR,FAILED,true"
    })
    void newlyPreparedSpotDoesNotRunTimerBeforeReadyPublication(
            systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode mode,
            PreparationOutcome outcome,
            boolean requestClose)
            throws Exception {
        ReadyTimerSpot.entered = new CompletableFuture<>();
        ReadyTimerSpot.creation = new CompletableFuture<>();
        ReadyTimerSpot.initialization = new CompletableFuture<>();
        ReadyTimerSpot.requestClose = requestClose;
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRelocationStore(new systems.zlink.framework.runtime.InMemoryRelocationStore());
        options.addRouteMesh("mesh")
                .listen("inproc://timer-ready-" + UUID.randomUUID())
                .objects()
                .server()
                .addSpotFactory(
                        "ready-timer",
                        ReadyTimerSpot.class,
                        factory -> {
                            factory.recreateOnRelocation();
                            factory.executionMode(mode);
                        });
        try (var controlled = new ControlledTimerRuntime(options)) {
            var scheduler = controlled.scheduler;
            var lifecycle = controlled.lifecycle;
            ZLinkSpotLifecycle.PreparedUserSpot prepared = null;
            CompletionStage<ZLinkSpotLifecycle.PreparedUserSpot> preparing = null;
            var publication = new CompletableFuture<Void>();
            CompletionStage<Void> published = null;
            try {
                preparing =
                        lifecycle.prepareReserved(
                                "mesh", ReadyTimerSpot.class, "ready-timer", 1, null);
                var spot = ReadyTimerSpot.entered.get(3, java.util.concurrent.TimeUnit.SECONDS);
                controlled.controlTimerLane(spot.context);
                var due = scheduler.due.poll(3, java.util.concurrent.TimeUnit.SECONDS);
                assertNotNull(due, "registered timer must have a platform expiration");
                due.run();
                assertFalse(
                        preparing.toCompletableFuture().isDone(),
                        "creation callback must still be held");
                assertEquals(0, spot.ticks, "timer callback started before Ready publication");
                if (requestClose) assertFalse(spot.closeRequested.toCompletableFuture().isDone());
                if (outcome == PreparationOutcome.REJECTED) {
                    ReadyTimerSpot.creation.complete(
                            systems.zlink.framework.spots.ZLinkSpotCreateResponse.reject());
                    prepared =
                            preparing
                                    .toCompletableFuture()
                                    .get(3, java.util.concurrent.TimeUnit.SECONDS);
                    assertFalse(prepared.created().response().accepted());
                    assertNull(prepared.created().activation());
                } else {
                    ReadyTimerSpot.creation.complete(
                            systems.zlink.framework.spots.ZLinkSpotCreateResponse.accept());
                    if (outcome == PreparationOutcome.FAILED) {
                        var failure = new IllegalStateException("initialization failed");
                        ReadyTimerSpot.initialization.completeExceptionally(failure);
                        var pending = preparing;
                        assertSame(
                                failure,
                                rootCause(
                                        assertThrows(
                                                CompletionException.class,
                                                () -> pending.toCompletableFuture().join())));
                    } else {
                        ReadyTimerSpot.initialization.complete(null);
                        prepared =
                                preparing
                                        .toCompletableFuture()
                                        .get(3, java.util.concurrent.TimeUnit.SECONDS);
                        var ready = prepared;
                        published =
                                publication.thenCompose(
                                        ignored -> lifecycle.publishReserved(ready));
                        assertFalse(
                                published.toCompletableFuture().isDone(),
                                "Ready publication must still be held");
                        assertEquals(
                                0, spot.ticks, "timer callback started before Ready publication");
                        if (requestClose)
                            assertFalse(spot.closeRequested.toCompletableFuture().isDone());
                        publication.complete(null);
                        published.toCompletableFuture().join();
                        spot.ticked.get(3, java.util.concurrent.TimeUnit.SECONDS);
                        assertEquals(
                                1, spot.ticks, "Ready publication must release the pending tick");
                    }
                }
                if (requestClose)
                    assertFalse(
                            spot.closeRequested
                                    .toCompletableFuture()
                                    .get(3, java.util.concurrent.TimeUnit.SECONDS),
                            "this synthetic reservation has no Store authority to Close");
                if (outcome != PreparationOutcome.READY) {
                    assertEquals(0, spot.ticks, "discarded timer callback must never start");
                }
            } finally {
                ReadyTimerSpot.creation.complete(
                        systems.zlink.framework.spots.ZLinkSpotCreateResponse.reject());
                ReadyTimerSpot.initialization.complete(null);
                publication.complete(null);
                if (published != null) published.toCompletableFuture().join();
                if (prepared == null
                        && preparing != null
                        && !preparing.toCompletableFuture().isCompletedExceptionally())
                    prepared = preparing.toCompletableFuture().join();
                if (prepared != null && prepared.created().activation() != null) {
                    if (lifecycle.spotActivationFor(prepared.spotId())
                            == prepared.created().activation())
                        lifecycle.retireClosed(prepared.created().activation());
                    lifecycle.discardReserved(prepared).toCompletableFuture().join();
                }
            }
        }
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.CsvSource({
        "false,false,none",
        "false,true,none",
        "true,false,none",
        "true,true,none",
        "false,true,cursor",
        "false,true,pointer",
        "false,true,initialize",
        "false,true,admission"
    })
    @SuppressWarnings("unchecked")
    void coldInstanceKeepsTimerAndCloseBehindFirstTerminalAndDurableRecovery(
            boolean handlerFails, boolean requestClose, String failedTransition) throws Exception {
        ReadyTimerInstance.initialized = new CompletableFuture<>();
        ReadyTimerInstance.initializeRelease = new CompletableFuture<>();
        ReadyTimerInstance.handlerStarted = new CompletableFuture<>();
        ReadyTimerInstance.handlerRelease = new CompletableFuture<>();
        ReadyTimerInstance.tickStarted = new CompletableFuture<>();
        ReadyTimerInstance.handlerFails = handlerFails;
        ReadyTimerInstance.requestClose = requestClose;
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRelocationStore(new systems.zlink.framework.runtime.InMemoryRelocationStore());
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        options.addRouteMesh("mesh")
                .listen("inproc://instance-timer-ready-" + UUID.randomUUID())
                .objects()
                .server()
                .addInstanceSpotFactory(
                        "ready-instance",
                        ReadyTimerInstance.class,
                        factory -> factory.disableRelocation());
        String spotId = "ready-instance-" + UUID.randomUUID();
        var cursorEntered = new CompletableFuture<Void>();
        var cursorRelease = new CompletableFuture<Void>();
        var pointerEntered = new CompletableFuture<Void>();
        var pointerRelease = new CompletableFuture<Void>();
        var recoveryFailure = new IllegalStateException("recovery " + failedTransition + " failed");
        try (var controlled = new ControlledTimerRuntime(options)) {
            var host = controlled.host;
            var original =
                    (systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository)
                            ControlledTimerRuntime.field(host, "userSpotLocationStore");
            var observed =
                    (systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository)
                            java.lang.reflect.Proxy.newProxyInstance(
                                    original.getClass().getClassLoader(),
                                    new Class<?>[] {
                                        systems.zlink.framework.runtime.internal.locations
                                                .ZLinkLocationRepository.class
                                    },
                                    (proxy, method, arguments) -> {
                                        CompletableFuture<Void> hold = null;
                                        String transition = "none";
                                        if (method.getName().equals("compareExchange")
                                                && systems.zlink.framework.runtime.locations
                                                        .ZLinkAuthorityKeyCodec.spot(spotId)
                                                        .equals(arguments[0])
                                                && arguments[2]
                                                        instanceof
                                                        systems.zlink.framework.runtime.internal
                                                                        .locations.ZLinkAuthorityPut
                                                                put) {
                                            var authority =
                                                    new systems.zlink.framework.runtime.locations
                                                                    .ZLinkServiceAuthorityPayloadCodec()
                                                            .decode(put.payload())
                                                            .orElseThrow();
                                            if (authority.state()
                                                    == systems.zlink.framework.runtime.locations
                                                            .ZLinkServiceAuthorityPayloadCodec.State
                                                            .READY) {
                                                if (authority
                                                        .activationRecoveryState()
                                                        .isPresent()) {
                                                    var pointer =
                                                            authority
                                                                    .activationRecoveryState()
                                                                    .orElseThrow();
                                                    if (pointer.replayCursor()
                                                            == pointer.inboxSequence()) {
                                                        cursorEntered.complete(null);
                                                        hold = cursorRelease;
                                                        transition = "cursor";
                                                    }
                                                } else {
                                                    pointerEntered.complete(null);
                                                    hold = pointerRelease;
                                                    transition = "pointer";
                                                }
                                            }
                                        }
                                        java.util.function.Supplier<CompletionStage<Object>> apply =
                                                () -> {
                                                    try {
                                                        return (CompletionStage<Object>)
                                                                method.invoke(original, arguments);
                                                    } catch (
                                                            java.lang.reflect
                                                                            .InvocationTargetException
                                                                    failure) {
                                                        return CompletableFuture.failedFuture(
                                                                failure.getCause());
                                                    } catch (IllegalAccessException failure) {
                                                        return CompletableFuture.failedFuture(
                                                                failure);
                                                    }
                                                };
                                        boolean failWrite =
                                                !failedTransition.equals("none")
                                                        && failedTransition.equals(transition);
                                        return hold == null
                                                ? apply.get()
                                                : hold.thenCompose(
                                                        ignored ->
                                                                failWrite
                                                                        ? CompletableFuture
                                                                                .failedFuture(
                                                                                        recoveryFailure)
                                                                        : apply.get());
                                    });
            controlled.replace(host, "userSpotLocationStore", observed);
            var first =
                    controlled
                            .framework
                            .route()
                            .requestToSpot(spotId, new InstanceProbe())
                            .instanceSpot("ready-instance")
                            .inMesh("mesh")
                            .timeout(Duration.ofSeconds(5))
                            .submit(InstanceReply.class)
                            .toCompletableFuture();
            try {
                var spot =
                        ReadyTimerInstance.initialized.get(
                                3, java.util.concurrent.TimeUnit.SECONDS);
                controlled.controlTimerLane(spot.context);
                var due = controlled.scheduler.due.poll(3, java.util.concurrent.TimeUnit.SECONDS);
                assertNotNull(due);
                due.run();
                assertEquals(0, spot.ticks);
                assertFalse(ReadyTimerInstance.handlerStarted.isDone());
                if (requestClose) assertFalse(spot.closeRequested.toCompletableFuture().isDone());
                if (failedTransition.equals("initialize") || failedTransition.equals("admission")) {
                    if (failedTransition.equals("initialize")) {
                        ReadyTimerInstance.initializeRelease.completeExceptionally(recoveryFailure);
                    } else {
                        controlled.replace(host, "draining", true);
                        ReadyTimerInstance.initializeRelease.complete(null);
                    }
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> first.get(3, java.util.concurrent.TimeUnit.SECONDS));
                    if (failedTransition.equals("initialize")) {
                        assertFalse(
                                spot.closeRequested
                                        .toCompletableFuture()
                                        .get(3, java.util.concurrent.TimeUnit.SECONDS));
                    } else {
                        var failure =
                                assertThrows(
                                        java.util.concurrent.ExecutionException.class,
                                        () ->
                                                spot.closeRequested
                                                        .toCompletableFuture()
                                                        .get(
                                                                3,
                                                                java.util.concurrent.TimeUnit
                                                                        .SECONDS));
                        assertEquals(
                                ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                                assertInstanceOf(ZLinkFrameworkException.class, rootCause(failure))
                                        .kind());
                    }
                    assertFalse(ReadyTimerInstance.handlerStarted.isDone());
                    assertFalse(cursorEntered.isDone());
                    assertFalse(pointerEntered.isDone());
                    assertEquals(0, spot.ticks);
                    return;
                }
                ReadyTimerInstance.initializeRelease.complete(null);
                ReadyTimerInstance.handlerStarted.get(3, java.util.concurrent.TimeUnit.SECONDS);
                assertEquals(
                        0,
                        spot.ticks,
                        "only the first durable record may enter before recovery completion");
                if (requestClose) assertFalse(spot.closeRequested.toCompletableFuture().isDone());
                ReadyTimerInstance.handlerRelease.complete(null);
                cursorEntered.get(3, java.util.concurrent.TimeUnit.SECONDS);
                assertEquals(0, spot.ticks);
                if (requestClose) assertFalse(spot.closeRequested.toCompletableFuture().isDone());
                cursorRelease.complete(null);
                if (!failedTransition.equals("cursor")) {
                    pointerEntered.get(3, java.util.concurrent.TimeUnit.SECONDS);
                    assertEquals(
                            0,
                            spot.ticks,
                            "durable cursor alone must not finish the recovery owner");
                    if (requestClose)
                        assertFalse(spot.closeRequested.toCompletableFuture().isDone());
                    pointerRelease.complete(null);
                }
                if (!failedTransition.equals("none")) {
                    var failedClose =
                            assertThrows(
                                    java.util.concurrent.ExecutionException.class,
                                    () ->
                                            spot.closeRequested
                                                    .toCompletableFuture()
                                                    .get(3, java.util.concurrent.TimeUnit.SECONDS));
                    assertSame(recoveryFailure, rootCause(failedClose));
                    assertEquals(0, spot.ticks);
                    var snapshot =
                            assertInstanceOf(
                                    systems.zlink.framework.runtime.internal.locations
                                            .ZLinkAuthoritySnapshot.class,
                                    original.read(
                                                    systems.zlink.framework.runtime.locations
                                                            .ZLinkAuthorityKeyCodec.spot(spotId),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .get(3, java.util.concurrent.TimeUnit.SECONDS));
                    var authority =
                            new systems.zlink.framework.runtime.locations
                                            .ZLinkServiceAuthorityPayloadCodec()
                                    .decode(snapshot.payload())
                                    .orElseThrow();
                    assertEquals(
                            systems.zlink.framework.runtime.locations
                                    .ZLinkServiceAuthorityPayloadCodec.State.READY,
                            authority.state());
                    assertTrue(
                            authority.activationRecoveryState().isPresent(),
                            "failed recovery must not move authority to Closing");
                } else if (requestClose) {
                    assertTrue(
                            spot.closeRequested
                                    .toCompletableFuture()
                                    .get(3, java.util.concurrent.TimeUnit.SECONDS));
                    assertEquals(
                            0,
                            spot.ticks,
                            "Close accepted before the timer disposes its pending registration");
                } else {
                    ReadyTimerInstance.tickStarted.get(3, java.util.concurrent.TimeUnit.SECONDS);
                    assertEquals(1, spot.ticks);
                }
                if (handlerFails) {
                    var failure =
                            assertThrows(
                                    java.util.concurrent.ExecutionException.class,
                                    () -> first.get(3, java.util.concurrent.TimeUnit.SECONDS));
                    assertTrue(failure.getCause().getMessage().contains("first handler failed"));
                } else
                    assertEquals(
                            new InstanceReply("first"),
                            first.get(3, java.util.concurrent.TimeUnit.SECONDS));
            } finally {
                ReadyTimerInstance.initializeRelease.complete(null);
                ReadyTimerInstance.handlerRelease.complete(null);
                cursorRelease.complete(null);
                pointerRelease.complete(null);
                first.handle((ignored, failure) -> null)
                        .get(3, java.util.concurrent.TimeUnit.SECONDS);
            }
        }
    }

    public record InstanceProbe() {}

    public record InstanceReply(String value) {}

    public static final class ReadyTimerInstance
            implements systems.zlink.framework.spots.ZLinkInstanceSpot {
        static CompletableFuture<ReadyTimerInstance> initialized;
        static CompletableFuture<Void> initializeRelease;
        static CompletableFuture<Void> handlerStarted;
        static CompletableFuture<Void> handlerRelease;
        static CompletableFuture<Void> tickStarted;
        static boolean handlerFails;
        static boolean requestClose;
        final systems.zlink.framework.spots.ZLinkInstanceSpotContext context;
        CompletionStage<Boolean> closeRequested;
        int ticks;

        public ReadyTimerInstance(systems.zlink.framework.spots.ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        @Override
        public systems.zlink.framework.spots.ZLinkInstanceSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addPacket(InstanceProbeHandler.class);
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            return context.addTimer(
                            "instance-ready", Duration.ofNanos(1), InstanceTickHandler.class, null)
                    .thenCompose(
                            timer -> {
                                if (requestClose) closeRequested = context.close();
                                initialized.complete(this);
                                return initializeRelease;
                            });
        }
    }

    public static final class InstanceProbeHandler
            implements systems.zlink.framework.spots.ZLinkSpotRequestHandler<
                    ReadyTimerInstance, InstanceProbe, InstanceReply> {
        @Override
        public CompletionStage<InstanceReply> handle(
                ReadyTimerInstance spot, InstanceProbe request) {
            ReadyTimerInstance.handlerStarted.complete(null);
            return ReadyTimerInstance.handlerRelease.thenCompose(
                    ignored ->
                            ReadyTimerInstance.handlerFails
                                    ? CompletableFuture.failedFuture(
                                            new IllegalStateException("first handler failed"))
                                    : CompletableFuture.completedFuture(
                                            new InstanceReply("first")));
        }
    }

    public static final class InstanceTickHandler {
        public CompletionStage<Void> handle(
                ReadyTimerInstance spot, systems.zlink.framework.spots.ZLinkTimerTick tick) {
            spot.ticks++;
            ReadyTimerInstance.tickStarted.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }

    enum PreparationOutcome {
        READY,
        REJECTED,
        FAILED
    }

    public static class ReadyTimerSpot implements ZLinkSpot<ZLinkActor> {
        private static CompletableFuture<ReadyTimerSpot> entered;
        private static CompletableFuture<systems.zlink.framework.spots.ZLinkSpotCreateResponse>
                creation;
        private static CompletableFuture<Void> initialization;
        private static boolean requestClose;
        private final ZLinkSpotContext context;
        final CompletableFuture<Void> ticked = new CompletableFuture<>();
        CompletionStage<Boolean> closeRequested;
        int ticks;

        public ReadyTimerSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<systems.zlink.framework.spots.ZLinkSpotCreateResponse> onCreate(
                systems.zlink.framework.messaging.ZLinkMessage request) {
            return context.addTimer("ready", Duration.ofNanos(1), ReadyTimerHandler.class, null)
                    .thenCompose(
                            timer -> {
                                if (requestClose) closeRequested = context.close();
                                entered.complete(this);
                                return creation;
                            });
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            return initialization;
        }
    }

    public static final class ReadyTimerHandler {
        public CompletionStage<Void> handle(
                ReadyTimerSpot spot, systems.zlink.framework.spots.ZLinkTimerTick tick) {
            spot.ticks++;
            spot.ticked.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class ControlledTimerRuntime implements AutoCloseable {
        final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime framework;
        final ZLinkSpotRuntime host;
        final ZLinkSpotLifecycle lifecycle;
        final ZLinkRelocatedTimerRebindTest.ManualScheduler scheduler =
                new ZLinkRelocatedTimerRebindTest.ManualScheduler();
        final DirectExecutor serial = new DirectExecutor();
        private final java.util.List<Runnable> restorations = new java.util.ArrayList<>();

        ControlledTimerRuntime(DefaultZLinkFrameworkOptions options) throws Exception {
            framework = ZLinkFrameworkRuntimeTestAccess.start(options);
            host = (ZLinkSpotRuntime) framework.spotManager();
            lifecycle = (ZLinkSpotLifecycle) field(host, "spotLifecycle");
            replace(host, "timerExecutor", scheduler);
            replace(host, "handlerExecutor", serial);
            replace(field(host, "frameworkRegistration"), "serialExecutor", serial);
        }

        void controlTimerLane(Object context) throws Exception {
            replace(field(field(context, "timers"), "stateLane"), "executor", serial);
        }

        private static Object field(Object owner, String name) throws Exception {
            var field = owner.getClass().getDeclaredField(name);
            field.setAccessible(true);
            return field.get(owner);
        }

        void replace(Object owner, String name, Object value) throws Exception {
            var field = owner.getClass().getDeclaredField(name);
            field.setAccessible(true);
            Object original = field.get(owner);
            field.set(owner, value);
            restorations.add(
                    () -> {
                        try {
                            field.set(owner, original);
                        } catch (IllegalAccessException failure) {
                            throw new CompletionException(failure);
                        }
                    });
        }

        @Override
        public void close() throws Exception {
            for (int index = restorations.size() - 1; index >= 0; index--)
                restorations.get(index).run();
            try {
                framework.close();
            } finally {
                scheduler.close();
                serial.close();
            }
        }
    }

    private static final class DirectExecutor extends java.util.concurrent.AbstractExecutorService {
        private boolean shutdown;

        @Override
        public void execute(Runnable work) {
            work.run();
        }

        @Override
        public void shutdown() {
            shutdown = true;
        }

        @Override
        public java.util.List<Runnable> shutdownNow() {
            shutdown = true;
            return java.util.List.of();
        }

        @Override
        public boolean isShutdown() {
            return shutdown;
        }

        @Override
        public boolean isTerminated() {
            return shutdown;
        }

        @Override
        public boolean awaitTermination(long timeout, java.util.concurrent.TimeUnit unit) {
            return shutdown;
        }
    }

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
    void initializeMismatchReportsRegistrationFailureAndCompletesTargetPublication()
            throws Exception {
        var reported = new CompletableFuture<String>();
        var logger =
                java.util.logging.Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer");
        var listener =
                new java.util.logging.Handler() {
                    @Override
                    public void publish(java.util.logging.LogRecord record) {
                        if (record.getMessage().contains("bad-initialize")
                                && record.getMessage().contains("error"))
                            reported.complete(record.getMessage());
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.addHandler(listener);
        try {
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
                        boolean published = false;
                        try {
                            var spot = (InitializeMismatchSpot) lifecycle.preparedSpot(prepared);
                            lifecycle
                                    .completeRelocationReady(prepared)
                                    .toCompletableFuture()
                                    .join();
                            var failure =
                                    assertThrows(
                                            CompletionException.class,
                                            () -> spot.registration.toCompletableFuture().join());
                            var original =
                                    assertInstanceOf(
                                            ZLinkFrameworkException.class, rootCause(failure));
                            assertEquals(
                                    ZLinkFrameworkErrorKind.INVALID_OPERATION, original.kind());
                            String diagnostic;
                            try {
                                diagnostic = reported.get(3, java.util.concurrent.TimeUnit.SECONDS);
                            } catch (Exception missing) {
                                throw new AssertionError(
                                        "initialization failure must be reported", missing);
                            }
                            assertTrue(diagnostic.contains(original.getMessage()));
                            var hold =
                                    lifecycle
                                            .beginReservedIngressHold(prepared)
                                            .toCompletableFuture()
                                            .join();
                            lifecycle.publishReserved(prepared).toCompletableFuture().join();
                            published = true;
                            lifecycle.publishReservedTimers(prepared);
                            lifecycle
                                    .resumeReservedIngress(prepared, hold)
                                    .toCompletableFuture()
                                    .join();
                            assertNotNull(spot.configured);
                            assertFalse(spot.context().close().toCompletableFuture().join());
                        } finally {
                            if (published) lifecycle.retireClosed(prepared.created().activation());
                            lifecycle.discardReserved(prepared).toCompletableFuture().join();
                        }
                    });
        } finally {
            logger.removeHandler(listener);
        }
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
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
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
        CompletionStage<ZLinkTimer> registration;

        public InitializeMismatchSpot(ZLinkSpotContext context) {
            super(context);
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            registration =
                    context()
                            .addTimer(
                                    "restored",
                                    Duration.ofDays(2),
                                    ZLinkRelocatedTimerRebindTest.OtherHandler.class,
                                    null);
            return registration.thenApply(ignored -> null);
        }
    }
}
