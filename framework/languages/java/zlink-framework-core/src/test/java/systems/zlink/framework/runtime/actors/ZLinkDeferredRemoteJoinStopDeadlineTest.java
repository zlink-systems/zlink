package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome;
import systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.net.ServerSocket;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkDeferredRemoteJoinStopDeadlineTest {
    private static final String ACTOR_ID = "deadline-player";
    private static final RoutingId SOURCE = RoutingId.from("deadline-source");
    private static final RoutingId TARGET = RoutingId.from("deadline-target");
    private static final String MESH = "join-deadline";
    private static final ZLinkStoreKey AUTHORITY =
            new ZLinkStoreKey("authority\0actor\0" + ACTOR_ID);
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private static final Duration STOP_BUDGET = Duration.ofMillis(400);
    private static Observations observations;

    @Test
    void unsettledRemoteJoinNotifiesTheSourceActorOnceAtTheStopDeadline() throws Exception {
        var evidence = new Observations();
        observations = evidence;
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var sourceStore = new SettlementStore(locations);
        var sourceBackend =
                ZLinkFrameworkRuntimeTestAccess.withMeshNodeDecorator(
                        new ZLinkJavaBackendAdapterFactory(),
                        node -> holdSettlementAfterPrepare(node, sourceStore, evidence));

        try (var source =
                        ZLinkFrameworkRuntimeTestAccess.startHost(
                                options(SOURCE, sourceStore, relocations), sourceBackend);
                var target =
                        ZLinkFrameworkRuntimeTestAccess.start(
                                options(TARGET, locations, relocations));
                AutoCloseable releaseGate =
                        () -> {
                            source.shutdown(STOP_BUDGET);
                            sourceStore.releaseForPreserve();
                        }) {
            ready(source);
            ready(target);
            var actor =
                    assertInstanceOf(
                            ZLinkActorCreateResult.Created.class,
                            await(source.actorManager().create(ACTOR_ID, "player").submit()));
            assertEquals(SOURCE, actor.actor().nodeRid());
            var sourceActors = ZLinkFrameworkRuntimeTestAccess.actors(source);
            evidence.sourceActor = (Player) sourceActors.localActor(ACTOR_ID).orElseThrow();
            await(target.spotManager().getOrCreate("room", "room").submit());
            awaitPeer(source);
            evidence.sourceLane = await(sourceActors.actorRelocationLaneAsync(ACTOR_ID));

            assertEquals(
                    "scheduled",
                    await(
                            source.actorClient()
                                    .requestToActor(ACTOR_ID, new Join())
                                    .submit(String.class)));
            await(sourceStore.settlementRead);
            var accepted =
                    assertInstanceOf(
                            ZLinkActorJoinCompletion.Accepted.class, await(evidence.target));
            assertEquals(TARGET, accepted.actor().nodeRid());
            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    await(
                            locations.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(), List.of(new ZLinkStoreDelete(AUTHORITY))),
                                    OPEN)));
            assertInstanceOf(ZLinkStoreReadMissing.class, await(locations.read(AUTHORITY, OPEN)));
            assertTrue(evidence.source.isEmpty(), "source callback before shutdown");

            assertFalse(evidence.backlog.isDone(), "source backlog held by the relocation seal");

            var stopping = source.shutdown(STOP_BUDGET);
            Instant deadline = source.status().deadline().orElseThrow();
            sourceStore.releaseForPreserve();
            Instant preserve = await(sourceStore.drainingRead);
            assertTrue(preserve.isBefore(deadline), "Preserve entered before the deadline");

            var stopped = await(stopping);
            assertAll(
                    () ->
                            assertEquals(
                                    ZLinkFrameworkTerminationOutcome.FORCE_STOPPED,
                                    stopped.outcome()),
                    () ->
                            assertEquals(
                                    ZLinkFrameworkTerminationReason.DEADLINE_EXCEEDED,
                                    stopped.reason()),
                    () -> assertEquals(1, evidence.source.size(), "source Join completion count"),
                    () -> {
                        var callback = evidence.source.stream().findFirst().orElse(null);
                        assertNotNull(callback, "source Join completion");
                        var failure =
                                assertInstanceOf(
                                        ZLinkActorJoinCompletion.Failed.class,
                                        callback.completion());
                        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, failure.kind());
                        assertEquals(accepted.operationId(), failure.operationId());
                        assertFalse(
                                callback.at().isBefore(deadline),
                                "source callback before deadline");
                        assertTrue(callback.backlogPending(), "source backlog at settlement");
                    },
                    () -> assertFalse(evidence.backlogRan.get(), "source backlog during shutdown"),
                    () ->
                            assertEquals(
                                    List.of(),
                                    sourceStore.actorCalls.stream()
                                            .filter(call -> !call.at().isBefore(deadline))
                                            .toList(),
                                    "source Actor Store calls after the stop deadline"),
                    () ->
                            assertInstanceOf(
                                    ZLinkStoreReadMissing.class,
                                    await(locations.read(AUTHORITY, OPEN))));
        }
    }

    private static ZLinkInternalMeshNode holdSettlementAfterPrepare(
            ZLinkInternalMeshNode node, SettlementStore store, Observations evidence) {
        return (ZLinkInternalMeshNode)
                Proxy.newProxyInstance(
                        ZLinkInternalMeshNode.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalMeshNode.class},
                        (proxy, method, arguments) -> {
                            try {
                                Object result = method.invoke(node, arguments);
                                if (method.getName().equals("requestCanonicalRelocationPrepare")) {
                                    return ((CompletionStage<?>) result)
                                            .thenApply(
                                                    ready -> {
                                                        evidence.holdBacklog();
                                                        store.holdSettlement.set(true);
                                                        return ready;
                                                    });
                                }
                                return result;
                            } catch (InvocationTargetException failure) {
                                throw failure.getCause();
                            }
                        });
    }

    private static void awaitPeer(ZLinkFrameworkRuntime source) throws Exception {
        var mesh = ZLinkFrameworkRuntimeTestAccess.meshNode(source, MESH);
        var ready = new CompletableFuture<Void>();
        Runnable observe =
                () -> {
                    if (mesh.observePeerRoute(TARGET).ready()) {
                        ready.complete(null);
                    }
                };
        try (var subscription = mesh.onPeerStateChanged(TARGET, observe, () -> {})) {
            observe.run();
            await(ready);
        }
    }

    private static void ready(ZLinkFrameworkRuntime runtime) throws Exception {
        await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime));
        assertTrue(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)));
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(5, TimeUnit.SECONDS);
    }

    private static DefaultZLinkFrameworkOptions options(
            RoutingId rid, ZLinkLocationStore store, InMemoryRelocationStore relocations)
            throws Exception {
        final int port;
        try (var socket = new ServerSocket(0)) {
            port = socket.getLocalPort();
        }
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRelocationStore(relocations);
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var mesh =
                options.addRouteMesh(MESH)
                        .setRoutingId(rid)
                        .listen("tcp://127.0.0.1:" + port)
                        .setPlacementWeight(SOURCE.equals(rid) ? 100 : 1);
        mesh.channelName(MESH).server();
        var objects = mesh.objects().server();
        objects.addEntrySpot(Entry.class);
        objects.addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
        objects.addActorFactory(
                "player", Player.class, Factory.class, factory -> factory.recreateOnRelocation());
        return options;
    }

    private static final class Observations {
        volatile Player sourceActor;
        final CompletableFuture<ZLinkActorJoinCompletion> target = new CompletableFuture<>();
        final List<SourceCompletion> source = new CopyOnWriteArrayList<>();
        final AtomicBoolean backlogRan = new AtomicBoolean();
        volatile ZLinkSerialExecutionQueue sourceLane;
        volatile CompletableFuture<Void> backlog;

        void holdBacklog() {
            backlog =
                    sourceLane
                            .enqueuePreviouslyAccepted(
                                    () -> {
                                        backlogRan.set(true);
                                        return CompletableFuture.completedFuture(null);
                                    })
                            .toCompletableFuture();
        }
    }

    private record SourceCompletion(
            ZLinkActorJoinCompletion completion, Instant at, boolean backlogPending) {}

    private record StoreCall(String operation, Instant at) {}

    private static final class SettlementStore implements ZLinkLocationStore {
        private final ZLinkLocationStore delegate;
        final AtomicBoolean holdSettlement = new AtomicBoolean();
        final CompletableFuture<Void> settlementRead = new CompletableFuture<>();
        final CompletableFuture<ZLinkStoreReadResult> settlementGate = new CompletableFuture<>();
        final CompletableFuture<Instant> drainingRead = new CompletableFuture<>();
        final List<StoreCall> actorCalls = new CopyOnWriteArrayList<>();

        SettlementStore(ZLinkLocationStore delegate) {
            this.delegate = delegate;
        }

        void releaseForPreserve() {
            settlementGate.completeExceptionally(
                    new IllegalStateException("settlement read failed"));
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            if (!key.equals(AUTHORITY) || !holdSettlement.get()) {
                return delegate.read(key, cancellation);
            }
            actorCalls.add(new StoreCall("read", Instant.now()));
            settlementRead.complete(null);
            if (!settlementGate.isDone()) {
                return settlementGate;
            }
            drainingRead.complete(Instant.now());
            return CompletableFuture.failedFuture(
                    new IllegalStateException("settlement read failed"));
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            if (holdSettlement.get()
                    && (request.mutations().stream()
                                    .anyMatch(mutation -> mutation.key().equals(AUTHORITY))
                            || request.conditions().stream()
                                    .anyMatch(condition -> condition.keys().contains(AUTHORITY)))) {
                Instant now = Instant.now();
                actorCalls.add(new StoreCall("write", now));
            }
            return delegate.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }
    }

    public record Join() {}

    public record Player(ZLinkActorContext context) implements ZLinkActor {
        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            if (this == observations.sourceActor) {
                var backlog = observations.backlog;
                observations.source.add(
                        new SourceCompletion(
                                completion, Instant.now(), backlog != null && !backlog.isDone()));
            } else {
                observations.target.complete(completion);
            }
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public record Entry(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void configure() {
            context.handlers().addHandler(EntryJoin.class);
        }
    }

    public static final class EntryJoin
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Entry entry, Player actor, ZLinkMessageContext context, Join request) {
            actor.context().joinSpot("room", request).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }
    }
}
