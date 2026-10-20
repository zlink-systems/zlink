package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.*;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeUnit;

final class ZLinkRelocatedTimerOutcomeTest {
    private static final String MESH = "timer-outcome";
    private static final Duration WAIT = Duration.ofSeconds(15);
    private static final Duration PERIOD = Duration.ofDays(1);
    private static Scenario scenario;

    enum Callback {
        CONFIGURE_PROPAGATES,
        CONFIGURE_IGNORES,
        INITIALIZE_PROPAGATES
    }

    @ParameterizedTest
    @EnumSource(Callback.class)
    void relocationOutcomeFollowsTheRegistrationCallbackCommitBoundary(Callback callback)
            throws Exception {
        scenario = new Scenario(callback);
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        try (var source =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options("source", locations, relocations, null))) {
            source.spotManager()
                    .getOrCreate("room", "room")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            String endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var target =
                            ZLinkFrameworkRuntimeTestAccess.start(
                                    options("target", locations, relocations, endpoint));
                    var sourceReady = new Ready(source.routeMeshRuntime().observe(MESH, 8));
                    var targetReady = new Ready(target.routeMeshRuntime().observe(MESH, 8))) {
                sourceReady.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                targetReady.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                var result =
                        source.relocate(
                                        new ZLinkFrameworkRelocationOptions(
                                                ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                                null,
                                                WAIT))
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                if (callback == Callback.CONFIGURE_PROPAGATES) {
                    assertEquals(ZLinkFrameworkRelocationOutcome.BLOCKED, result.outcome());
                    assertEquals(ZLinkFrameworkRelocationReason.RELOCATION_FAILED, result.reason());
                    assertFalse(scenario.sourceTimer.isDisposed());
                } else {
                    assertEquals(ZLinkFrameworkRelocationOutcome.RELOCATED, result.outcome());
                    assertEquals(ZLinkFrameworkRelocationReason.NONE, result.reason());
                    assertTrue(scenario.sourceTimer.isDisposed(), "source must not resume");
                    var dispatched = new java.util.concurrent.atomic.AtomicBoolean();
                    scenario.target
                            .context
                            .enqueueDispatch(
                                    () -> {
                                        dispatched.set(true);
                                        return CompletableFuture.completedFuture(null);
                                    })
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    assertTrue(
                            dispatched.get(),
                            "target local admission must open after its callback completes");
                    assertTrue(
                            ((ZLinkSpotRuntime) target.spotManager())
                                    .activeUserSpotIds()
                                    .contains("room"));
                    var restored = scenario.target.context.freezeTimerRelocationEnvelope();
                    assertEquals(
                            ZLinkRelocatedTimerRebindTest.TickHandler.class.getName(),
                            ZLinkSpotTimerRelocationEnvelope.canonicalize(restored)
                                    .getFirst()
                                    .handlerType());
                    assertFalse(scenario.target.restored.isDisposed());
                }
                var failure = assertThrows(CompletionException.class, scenario.registration::join);
                assertEquals(
                        ZLinkFrameworkErrorKind.INVALID_OPERATION,
                        assertInstanceOf(ZLinkFrameworkException.class, rootCause(failure)).kind());
            }
        }
    }

    private static Throwable rootCause(Throwable failure) {
        while (failure.getCause() != null) failure = failure.getCause();
        return failure;
    }

    private static DefaultZLinkFrameworkOptions options(
            String rid,
            ZLinkInMemoryLocationStore locations,
            InMemoryRelocationStore relocations,
            String peer) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var node =
                options.addRouteMesh(MESH)
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from(rid));
        if (peer != null) node.peerConnections().connect(peer);
        node.objects().server().addSpotFactory("room", Room.class, f -> f.recreateOnRelocation());
        return options;
    }

    private static final class Scenario {
        final Callback callback;
        final CompletableFuture<ZLinkTimer> registration = new CompletableFuture<>();
        ZLinkTimer sourceTimer;
        Room target;

        Scenario(Callback callback) {
            this.callback = callback;
        }
    }

    private static final class Ready
            implements Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>>, AutoCloseable {
        final CompletableFuture<Void> result = new CompletableFuture<>();
        Flow.Subscription subscription;

        Ready(Flow.Publisher<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> publisher) {
            publisher.subscribe(this);
        }

        @Override
        public void onSubscribe(Flow.Subscription subscription) {
            this.subscription = subscription;
            subscription.request(Long.MAX_VALUE);
        }

        @Override
        public void onNext(ZLinkObservedStatus<ZLinkMeshNodeSnapshot> value) {
            if (value.status().isReady() && value.status().readyPeerCount() > 0)
                result.complete(null);
        }

        @Override
        public void onError(Throwable failure) {
            result.completeExceptionally(failure);
        }

        @Override
        public void onComplete() {}

        @Override
        public void close() {
            subscription.cancel();
        }
    }

    public static final class Room implements ZLinkSpot<ZLinkActor> {
        final DefaultSpotContext context;
        ZLinkTimer restored;

        public Room(ZLinkSpotContext context) {
            this.context = (DefaultSpotContext) context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            restored =
                    context.addTimer(
                                    "restored",
                                    PERIOD,
                                    ZLinkRelocatedTimerRebindTest.TickHandler.class,
                                    null)
                            .toCompletableFuture()
                            .join();
            if (context.nodeRid().equals(RoutingId.from("source"))) {
                scenario.sourceTimer = restored;
            } else {
                scenario.target = this;
                if (scenario.callback != Callback.INITIALIZE_PROPAGATES) {
                    var registration = mismatch();
                    if (scenario.callback == Callback.CONFIGURE_PROPAGATES)
                        registration.toCompletableFuture().join();
                }
            }
        }

        private CompletionStage<ZLinkTimer> mismatch() {
            return context.addTimer(
                            "restored",
                            PERIOD,
                            ZLinkRelocatedTimerRebindTest.OtherHandler.class,
                            null)
                    .whenComplete(
                            (timer, failure) -> {
                                if (failure == null) scenario.registration.complete(timer);
                                else scenario.registration.completeExceptionally(failure);
                            });
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            if (context.nodeRid().equals(RoutingId.from("target"))
                    && scenario.callback == Callback.INITIALIZE_PROPAGATES)
                return mismatch().thenApply(ignored -> null);
            return CompletableFuture.completedFuture(null);
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
}
