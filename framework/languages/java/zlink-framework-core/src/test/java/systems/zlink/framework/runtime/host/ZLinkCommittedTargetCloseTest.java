package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import static systems.zlink.framework.runtime.host.ZLinkRelocationObservationPublicTest.WAIT;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntimeTestAccess;
import systems.zlink.framework.spots.*;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkCommittedTargetCloseTest {
    private static final String MESH = "committed-target-close";
    private static final RoutingId TARGET = RoutingId.from("publication-target");
    private static final List<Boolean> TARGET_CLOSING = new CopyOnWriteArrayList<>();
    private static CompletableFuture<Void> publicationRelease;
    private static CompletableFuture<?> publicationTerminal;

    @Test
    void closeWaitsForCommittedTargetPublicationBeforeClosingSpotScopes() throws Exception {
        TARGET_CLOSING.clear();
        publicationRelease = new CompletableFuture<>();
        publicationTerminal = null;
        var entered = new CompletableFuture<Void>();
        var rejectedClose = new CompletableFuture<Void>();
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var logger = Logger.getLogger("systems.zlink.framework");
        var handler =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        if (String.valueOf(record.getMessage())
                                .contains("queue relocation is in progress"))
                            rejectedClose.complete(null);
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.addHandler(handler);
        try (var source =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options(
                                RoutingId.from("publication-source"),
                                locations,
                                relocations,
                                null))) {
            source.spotManager()
                    .getOrCreate("held-room", "room")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            var targetOptions = options(TARGET, locations, relocations, endpoint);
            try (var target = ZLinkFrameworkRuntimeTestAccess.start(targetOptions)) {
                installTargetRetireRuntime(
                        target,
                        targetOptions,
                        normalize ->
                                request ->
                                        normalize
                                                .apply(request)
                                                .thenCompose(
                                                        ignored -> {
                                                            entered.complete(null);
                                                            return publicationRelease;
                                                        }));
                try (var ready =
                        ZLinkRelocationObservationPublicTest.observe(
                                target.routeMeshRuntime().observe(MESH, 8),
                                status -> status.isReady() && status.readyPeerCount() > 0)) {
                    ready.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                }
                var retire = ZLinkFrameworkRuntimeTestAccess.spotRetire(target);
                assertEquals(1, ZLinkSpotRuntimeTestAccess.relocationStateMachineCount(retire));
                var machine = ZLinkSpotRuntimeTestAccess.firstRelocationStateMachine(retire);
                var release = publicationRelease;
                try {
                    var relocation =
                            source.relocate(
                                    new ZLinkFrameworkRelocationOptions(
                                            ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                            null,
                                            WAIT));
                    entered.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    assertEquals(
                            ZLinkFrameworkRelocationOutcome.RELOCATED,
                            relocation
                                    .toCompletableFuture()
                                    .get(WAIT.toSeconds(), TimeUnit.SECONDS)
                                    .outcome());
                    // The real Steady write finished, but this accepted target still owns ingress.
                    var terminal = ZLinkSpotRuntimeTestAccess.firstTargetAttemptTerminal(machine);
                    assertEquals(1, ZLinkSpotRuntimeTestAccess.targetAttemptCount(machine));
                    assertFalse(terminal.isDone());
                    publicationTerminal = terminal;
                    int observers = terminal.getNumberOfDependents();
                    var closed = target.closeAsync().toCompletableFuture();
                    try {
                        long until = System.nanoTime() + WAIT.toNanos();
                        // Synchronize with the existing terminal join or an actual close rejection.
                        while (!closed.isDone()
                                && !rejectedClose.isDone()
                                && terminal.getNumberOfDependents() <= observers) {
                            assertTrue(
                                    System.nanoTime() < until,
                                    "close did not reach the target terminal or its rejection");
                            Thread.onSpinWait();
                        }
                    } finally {
                        release.complete(null);
                    }
                    assertDoesNotThrow(() -> closed.get(WAIT.toSeconds(), TimeUnit.SECONDS));
                    assertEquals(
                            List.of(true),
                            TARGET_CLOSING,
                            "target OnClosing must follow accepted publication terminal");
                } finally {
                    release.complete(null);
                }
            }
        } finally {
            publicationRelease.complete(null);
            logger.removeHandler(handler);
        }
    }

    private static DefaultZLinkFrameworkOptions options(
            RoutingId rid,
            ZLinkInMemoryLocationStore locations,
            InMemoryRelocationStore relocations,
            String peer) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var node = options.addRouteMesh(MESH).listen("tcp://127.0.0.1:0").setRoutingId(rid);
        if (peer != null) node.peerConnections().connect(peer);
        node.objects()
                .server()
                .addSpotFactory("room", Room.class, factory -> factory.recreateOnRelocation());
        return options;
    }

    private static void installTargetRetireRuntime(
            ZLinkFrameworkRuntime runtime,
            DefaultZLinkFrameworkOptions options,
            java.util.function.UnaryOperator<
                            java.util.function.Function<Object, CompletionStage<Void>>>
                    normalizerDecorator) {
        var registration = options.registration();
        var locations = ZLinkFrameworkRuntimeTestAccess.authorityStore(runtime);
        var retireRuntime =
                ZLinkSpotRuntimeTestAccess.userSpotRetireRuntimeWithStateMachines(
                        (systems.zlink.framework.runtime.spots.ZLinkSpotRuntime)
                                runtime.spotManager(),
                        (systems.zlink.framework.runtime.actors.ZLinkActorRuntime)
                                runtime.actorManager(),
                        registration.meshNodes(),
                        ZLinkFrameworkRuntimeTestAccess.meshNodes(runtime),
                        locations,
                        ZLinkFrameworkRuntimeTestAccess.storeLocationResolvers(runtime),
                        locations,
                        registration.locations().options(),
                        new systems.zlink.framework.runtime.internal.relocation
                                .ZLinkRelocationAdapterRegistry(
                                registration,
                                systems.zlink.framework.runtime.internal.handlers
                                        .ZLinkHandlerActivator.reflection()),
                        registration.applicationVersion(),
                        registration.maintenanceWave(),
                        registration.locations().options().sessionRelocationSealTimeout(),
                        ZLinkFrameworkRuntimeTestAccess.meshDrains(runtime)::isSealed,
                        null,
                        null,
                        normalizerDecorator);
        ZLinkFrameworkRuntimeTestAccess.replaceSpotRetire(runtime, retireRuntime);
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<ZLinkActor> {
        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
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
        public CompletionStage<Void> onClosing(ZLinkSpotClosingContext closing) {
            if (context.nodeRid().equals(TARGET))
                TARGET_CLOSING.add(publicationTerminal != null && publicationTerminal.isDone());
            return CompletableFuture.completedFuture(null);
        }
    }
}
