package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.*;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.spots.*;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkStreamError;

import java.net.Socket;
import java.net.URI;
import java.time.Duration;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.*;
import java.util.function.Predicate;

/**
 * #1882: a bound Actor that moves zone B -> zone A -> zone B returns to the node that held its
 * first stay. The committed first move retired zone B's binding record of that stay, so the return
 * installs the relocating route, sends command 44 and the Session seal never times out.
 */
final class ZLinkBoundActorReturnTest {
    private static final String MESH = "bound-return";
    private static final String FLOW = "01900000-0000-7000-8000-000000001882";
    private static final Duration WAIT = Duration.ofSeconds(15);
    private static volatile CompletableFuture<ZLinkActorJoinCompletion> joined;
    private static volatile CompletableFuture<Void> scopeEntered;
    private static volatile CompletableFuture<Void> scopeRelease;
    private static final CompletableFuture<ZLinkSessionContext> connected =
            new CompletableFuture<>();
    private static final CompletableFuture<String> relayed = new CompletableFuture<>();

    @Test
    void boundActorReturnsToItsFirstNodeWithoutSealTimeout() throws Exception {
        joined = new CompletableFuture<>();
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        try (var zoneBHost =
                new Bounded(
                        ZLinkFrameworkRuntimeTestAccess.start(
                                options("zone-b", locations, relocations, null)))) {
            var zoneB = zoneBHost.runtime();
            var roomB =
                    zoneB.spotManager()
                            .getOrCreate("room-b", "room")
                            .submit()
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            var actor =
                    assertInstanceOf(
                                    ZLinkActorCreateResult.Created.class,
                                    zoneB.actorManager()
                                            .create("player-return", "player")
                                            .submit()
                                            .toCompletableFuture()
                                            .get(WAIT.toSeconds(), TimeUnit.SECONDS))
                            .actor();
            assertEquals(RoutingId.from("zone-b"), roomB.spot().nodeRid());
            ((systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions)
                            zoneB.routeMeshRuntime())
                    .mesh(MESH)
                    .setPlacementWeight(0);
            var endpointB = zoneB.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var zoneAHost =
                            new Bounded(
                                    ZLinkFrameworkRuntimeTestAccess.start(
                                            options("zone-a", locations, relocations, endpointB)));
                    var zoneAReady =
                            observe(
                                    zoneAHost.runtime().routeMeshRuntime().observe(MESH, 8),
                                    s -> s.isReady() && s.readyPeerCount() > 0)) {
                zoneAReady.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                var zoneA = zoneAHost.runtime();
                var roomA =
                        zoneA.spotManager()
                                .getOrCreate("room-a", "room")
                                .submit()
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertEquals(RoutingId.from("zone-a"), roomA.spot().nodeRid());
                var endpointA = zoneA.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
                var gatewayOptions = new DefaultZLinkFrameworkOptions();
                gatewayOptions.addLocationStore(locations);
                gatewayOptions.addRelocationStore(relocations);
                gatewayOptions.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
                var gatewayMesh =
                        gatewayOptions
                                .addRouteMesh(MESH)
                                .listen("tcp://127.0.0.1:0")
                                .setRoutingId(RoutingId.from("gateway-return"));
                gatewayMesh.objects().client();
                gatewayMesh.peerConnections().connect(endpointB);
                gatewayMesh.peerConnections().connect(endpointA);
                var streamOptions = gatewayOptions.addStreamNode("gateway");
                streamOptions.bind("tcp://127.0.0.1:0");
                streamOptions.enableActorDispatch();
                streamOptions.registerSession(BoundSession.class);
                try (var gatewayHost =
                        new Bounded(ZLinkFrameworkRuntimeTestAccess.start(gatewayOptions))) {
                    var gateway = gatewayHost.runtime();
                    var address =
                            URI.create(
                                    gateway.listenerStatus(ZLinkListenerKind.STREAM, "gateway")
                                            .endpoint());
                    try (var client = new Socket(address.getHost(), address.getPort())) {
                        var session = connected.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                        var sessionActors = (ZLinkSessionActorsRuntime) session.actors();
                        var binding =
                                sessionActors
                                        .bind(actor)
                                        .toCompletableFuture()
                                        .get(WAIT.toSeconds(), TimeUnit.SECONDS);

                        // zone B -> zone A, then back to zone B (the node of the first stay).
                        assertEquals(
                                RoutingId.from("zone-a"),
                                moveTo(zoneB, "room-a").actor().nodeRid());
                        assertEquals(
                                RoutingId.from("zone-b"),
                                moveTo(zoneB, "room-b").actor().nodeRid());

                        sessionActors.enterRelayDispatch(
                                new ZLinkStreamHeader(
                                                Probe.class.getSimpleName(),
                                                Map.of(),
                                                Optional.empty())
                                        .withFlow(FLOW, ZLinkFlowOrigin.INBOUND));
                        CompletableFuture<Void> submitted;
                        try {
                            submitted =
                                    binding.relay(ZLinkMessage.of(new Probe("after-return")))
                                            .toCompletableFuture();
                        } finally {
                            sessionActors.exitRelayDispatch();
                        }
                        submitted.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                        assertEquals(
                                "after-return", relayed.get(WAIT.toSeconds(), TimeUnit.SECONDS));
                        assertTrue(sessionActors.bound().contains(binding));
                    }
                }
            }
        }
    }

    @Test
    void returnedActorCanDepartAgainWhenActorLeftWasDropped() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var firstLeft = new CompletableFuture<Void>();
        var secondLeft = new CompletableFuture<Void>();
        var dropped = new java.util.concurrent.atomic.AtomicInteger();
        var backend = droppingActorLeftBackend(dropped, firstLeft, secondLeft);
        try (var sourceHost =
                new Bounded(
                        ZLinkFrameworkRuntimeTestAccess.start(
                                options("repeat-b", locations, relocations, null), backend))) {
            var source = sourceHost.runtime();
            var roomB =
                    source.spotManager()
                            .getOrCreate("room-b", "room")
                            .submit()
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            assertEquals(RoutingId.from("repeat-b"), roomB.spot().nodeRid());
            source.actorManager()
                    .create("player-return", "player")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            ((systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions)
                            source.routeMeshRuntime())
                    .mesh(MESH)
                    .setPlacementWeight(0);
            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var targetHost =
                            new Bounded(
                                    ZLinkFrameworkRuntimeTestAccess.start(
                                            options("repeat-a", locations, relocations, endpoint),
                                            backend));
                    var ready =
                            observe(
                                    targetHost.runtime().routeMeshRuntime().observe(MESH, 8),
                                    status -> status.isReady() && status.readyPeerCount() > 0)) {
                ready.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                var roomA =
                        targetHost
                                .runtime()
                                .spotManager()
                                .getOrCreate("room-a", "room")
                                .submit()
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertEquals(RoutingId.from("repeat-a"), roomA.spot().nodeRid());
                assertEquals(
                        RoutingId.from("repeat-a"), moveTo(source, "room-a").actor().nodeRid());
                firstLeft.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertEquals(
                        RoutingId.from("repeat-b"), moveTo(source, "room-b").actor().nodeRid());
                secondLeft.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertEquals(
                        RoutingId.from("repeat-a"), moveTo(source, "room-a").actor().nodeRid());
            }
        }
    }

    private static systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider
            droppingActorLeftBackend(
                    java.util.concurrent.atomic.AtomicInteger dropped,
                    CompletableFuture<Void> firstLeft,
                    CompletableFuture<Void> secondLeft) {
        var delegate = new systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory();
        return (systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider)
                java.lang.reflect.Proxy.newProxyInstance(
                        systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider
                                .class
                                .getClassLoader(),
                        new Class<?>[] {
                            systems.zlink.framework.runtime.internal.backend
                                    .ZLinkBackendAdapterProvider.class
                        },
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("createMeshAdapter")) {
                                var adapter =
                                        delegate.createMeshAdapter(
                                                (systems.zlink.framework.runtime.internal.backend
                                                                .ZLinkBackendAdapterOptions)
                                                        arguments[0]);
                                return (systems.zlink.framework.runtime.internal.backend
                                                .ZLinkMeshBackendAdapter)
                                        (context, mesh) -> {
                                            var node = adapter.createMeshNode(context, mesh);
                                            return (systems.zlink.framework.runtime.internal.backend
                                                            .ZLinkInternalMeshNode)
                                                    java.lang.reflect.Proxy.newProxyInstance(
                                                            node.getClass().getClassLoader(),
                                                            node.getClass().getInterfaces(),
                                                            (meshProxy,
                                                                    meshMethod,
                                                                    meshArguments) -> {
                                                                if (meshMethod
                                                                        .getName()
                                                                        .equals("sendActorLeft")) {
                                                                    int count =
                                                                            dropped
                                                                                    .incrementAndGet();
                                                                    if (count == 1)
                                                                        firstLeft.complete(null);
                                                                    if (count == 2)
                                                                        secondLeft.complete(null);
                                                                    return CompletableFuture
                                                                            .completedFuture(null);
                                                                }
                                                                try {
                                                                    return meshMethod.invoke(
                                                                            node, meshArguments);
                                                                } catch (
                                                                        java.lang.reflect
                                                                                        .InvocationTargetException
                                                                                failure) {
                                                                    throw failure.getCause();
                                                                }
                                                            });
                                        };
                            }
                            try {
                                return method.invoke(delegate, arguments);
                            } catch (java.lang.reflect.InvocationTargetException failure) {
                                throw failure.getCause();
                            }
                        });
    }

    @Test
    void inboundPreparationWaitsForTheDepartingSourceCleanup() throws Exception {
        scopeEntered = new CompletableFuture<>();
        scopeRelease = new CompletableFuture<>();
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var armed = new java.util.concurrent.atomic.AtomicBoolean();
        var entered = new CompletableFuture<Void>();
        var release = new CompletableFuture<Void>();
        var authority =
                systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec.decode(
                        systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec.actor(
                                "player-return"));
        var actorKey =
                new systems.zlink.framework.locationprovider.ZLinkStoreKey(
                        "authority\0" + authority.kind() + "\0" + authority.id());
        var sourceStore =
                (systems.zlink.framework.locationprovider.ZLinkLocationStore)
                        java.lang.reflect.Proxy.newProxyInstance(
                                systems.zlink.framework.locationprovider.ZLinkLocationStore.class
                                        .getClassLoader(),
                                new Class<?>[] {
                                    systems.zlink.framework.locationprovider.ZLinkLocationStore
                                            .class
                                },
                                (proxy, method, arguments) -> {
                                    Object result;
                                    try {
                                        result = method.invoke(locations, arguments);
                                    } catch (java.lang.reflect.InvocationTargetException failure) {
                                        throw failure.getCause();
                                    }
                                    if (armed.get()
                                            && method.getName().equals("read")
                                            && actorKey.equals(arguments[0])
                                            && StackWalker.getInstance()
                                                    .walk(
                                                            frames ->
                                                                    frames.anyMatch(
                                                                            frame ->
                                                                                    frame.getClassName()
                                                                                                    .endsWith(
                                                                                                            ".ZLinkCanonicalRelocationStateMachine")
                                                                                            && frame.getMethodName()
                                                                                                    .equals(
                                                                                                            "settleSource")))) {
                                        entered.complete(null);
                                        return release.thenCompose(
                                                ignored -> (CompletionStage) result);
                                    }
                                    return result;
                                });
        try (var sourceHost =
                new Bounded(
                        ZLinkFrameworkRuntimeTestAccess.start(
                                options("cleanup-source", sourceStore, relocations, null)))) {
            var source = sourceHost.runtime();
            var sourceActors =
                    (systems.zlink.framework.runtime.actors.ZLinkActorRuntime)
                            source.actorManager();
            source.actorManager()
                    .create("player-return", "player")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            var sourceActor = sourceActors.localActor("player-return").orElseThrow();
            var sourceRef = sourceActors.actorRef(sourceActor);
            systems.zlink.framework.runtime.internal.handlers.ZLinkActorHandlerInstances.instance(
                    sourceActor, HeldSourceHandler.class);
            ((systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions)
                            source.routeMeshRuntime())
                    .mesh(MESH)
                    .setPlacementWeight(0);
            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var targetHost =
                            new Bounded(
                                    ZLinkFrameworkRuntimeTestAccess.start(
                                            options(
                                                    "cleanup-target",
                                                    locations,
                                                    relocations,
                                                    endpoint)));
                    var targetReady =
                            observe(
                                    targetHost.runtime().routeMeshRuntime().observe(MESH, 8),
                                    status -> status.isReady() && status.readyPeerCount() > 0)) {
                targetReady.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                var roomA =
                        targetHost
                                .runtime()
                                .spotManager()
                                .getOrCreate("room-a", "room")
                                .submit()
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertEquals(RoutingId.from("cleanup-target"), roomA.spot().nodeRid());
                systems.zlink.framework.runtime.actors.ZLinkActorRuntime.PreparedTransferredActor
                        prepared = null;
                try {
                    armed.set(true);
                    assertEquals(
                            RoutingId.from("cleanup-target"),
                            moveTo(source, "room-a").actor().nodeRid());
                    entered.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    var preparation =
                            sourceActors
                                    .prepareRelocatedActor(
                                            "player-return",
                                            "player",
                                            new byte[0],
                                            false,
                                            null,
                                            () -> false,
                                            sourceRef)
                                    .toCompletableFuture();
                    assertFalse(
                            preparation.isDone(),
                            "inbound preparation must wait for the previous source cleanup");
                    release.complete(null);
                    scopeEntered.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    assertTrue(
                            sourceActors.localActor("player-return").isEmpty(),
                            "commit must retire source membership before old scope disposal finishes");
                    assertFalse(
                            preparation.isDone(),
                            "fresh restore must wait for the old scope disposal terminal");
                    scopeRelease.complete(null);
                    prepared = preparation.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    assertNotSame(sourceActor, prepared.actor());
                } finally {
                    release.complete(null);
                    scopeRelease.complete(null);
                    if (prepared != null)
                        sourceActors
                                .discardPreparedTransferredActor(prepared)
                                .toCompletableFuture()
                                .join();
                }
            }
        }
    }

    @Test
    void oldFollowCleanupPreservesTheReturnedActorsAuthority() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        try (var host =
                new Bounded(
                        ZLinkFrameworkRuntimeTestAccess.start(
                                options("stale-follow", locations, relocations, null)))) {
            var runtime =
                    (systems.zlink.framework.runtime.actors.ZLinkActorRuntime)
                            host.runtime().actorManager();
            var created =
                    assertInstanceOf(
                            ZLinkActorCreateResult.Created.class,
                            host.runtime()
                                    .actorManager()
                                    .create("stale-follow-actor", "player")
                                    .submit()
                                    .toCompletableFuture()
                                    .get(WAIT.toSeconds(), TimeUnit.SECONDS));
            var actor = runtime.localActor(created.actor().actorId()).orElseThrow();
            var actorRef = runtime.actorRef(actor);
            var nodeField = runtime.getClass().getDeclaredField("spotNode");
            nodeField.setAccessible(true);
            var node =
                    (systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode)
                            nodeField.get(runtime);
            long oldAuthority = node.actorAuthorityOwnerGeneration(actorRef);
            long ownerLease = node.actorAuthorityOwnerLeaseGeneration(actorRef);
            var source =
                    new systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                            .ActorRouteFence(
                            actorRef, node.actorNodeGeneration(actorRef), oldAuthority, ownerLease);
            var targetRef =
                    new systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef(
                            RoutingId.from("follow-target"),
                            actorRef.actorId(),
                            actorRef.generation());
            var target =
                    new systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                            .ActorRouteFence(
                            targetRef,
                            node.actorNodeGeneration(actorRef),
                            oldAuthority + 1,
                            ownerLease);
            runtime.stageRelocationMessageFollowAsync(
                            source, targetRef, CompletableFuture.completedFuture(target))
                    .toCompletableFuture()
                    .join();
            var handoffField = runtime.getClass().getDeclaredField("handoff");
            handoffField.setAccessible(true);
            var handoff = handoffField.get(runtime);
            var route =
                    new systems.zlink.framework.runtime.internal.service
                            .ZLinkServiceMessageFollowWireCodec.ActorRoute(
                            actorRef.actorId(),
                            actorRef.generation(),
                            actorRef.nodeRid(),
                            node.actorNodeGeneration(actorRef),
                            oldAuthority,
                            ownerLease);
            var lookup =
                    handoff.getClass().getDeclaredMethod("messageFollowSource", route.getClass());
            lookup.setAccessible(true);
            var retained = ((Optional<?>) lookup.invoke(handoff, route)).orElseThrow();
            try {
                // Model the same Actor returning: ObjectGeneration stays unchanged; authority
                // advances.
                long returnedAuthority = oldAuthority + 2;
                node.rememberActorAuthority(actorRef, returnedAuthority, ownerLease);
                var cleanup =
                        runtime.getClass()
                                .getDeclaredMethod(
                                        "removeMessageFollowSource", retained.getClass());
                cleanup.setAccessible(true);
                cleanup.invoke(runtime, retained);
                runtime.applyRemoteSessionBinding(node, actorRef, ignored -> true)
                        .toCompletableFuture()
                        .join();
                assertEquals(
                        returnedAuthority,
                        node.actorAuthorityOwnerGeneration(actorRef),
                        "old Message Follow cleanup must preserve the returned Actor's native authority");
            } finally {
                runtime.abortRelocationMessageFollowAsync(source).toCompletableFuture().join();
            }
        }
    }

    /**
     * Shuts a host down within a bound, so a failed move reports its own assertion instead of
     * waiting forever on the shutdown of a host the failure left in Error.
     */
    private record Bounded(ZLinkFrameworkRuntime runtime) implements AutoCloseable {
        @Override
        public void close() throws Exception {
            runtime.shutdown(WAIT)
                    .toCompletableFuture()
                    .get(2 * WAIT.toSeconds(), TimeUnit.SECONDS);
            runtime.close();
        }
    }

    private static ZLinkActorJoinCompletion.Accepted moveTo(
            ZLinkFrameworkRuntime caller, String spotId) throws Exception {
        joined = new CompletableFuture<>();
        assertEquals(
                "scheduled",
                caller.actorClient()
                        .requestToActor("player-return", new Join(spotId))
                        .timeout(WAIT)
                        .submit(String.class)
                        .toCompletableFuture()
                        .get(WAIT.toSeconds(), TimeUnit.SECONDS));
        return assertInstanceOf(
                ZLinkActorJoinCompletion.Accepted.class,
                joined.get(WAIT.toSeconds(), TimeUnit.SECONDS),
                "Join to " + spotId);
    }

    private static DefaultZLinkFrameworkOptions options(
            String rid,
            systems.zlink.framework.locationprovider.ZLinkLocationStore locations,
            InMemoryRelocationStore relocations,
            String peer) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.configureLocations().setRouteCacheMaxAge(Duration.ZERO);
        // Longer than the test: only the commit may retire the first stay's record.
        options.configureLocations().setMessageFollowDuration(Duration.ofSeconds(30));
        var node =
                options.addRouteMesh(MESH)
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from(rid));
        if (peer != null) node.peerConnections().connect(peer);
        var objects = node.objects().server();
        objects.addEntrySpot(Lobby.class);
        objects.addActorFactory(
                "player",
                Player.class,
                PlayerFactory.class,
                f -> f.preserveStateWith(PlayerState.class));
        objects.addSpotFactory("room", Room.class, f -> f.recreateOnRelocation());
        return options;
    }

    private static <T> Observation<T> observe(
            java.util.concurrent.Flow.Publisher<ZLinkObservedStatus<T>> publisher,
            Predicate<T> predicate) {
        var observation = new Observation<T>(predicate);
        publisher.subscribe(observation);
        return observation;
    }

    private static final class Observation<T>
            implements java.util.concurrent.Flow.Subscriber<ZLinkObservedStatus<T>>, AutoCloseable {
        final CompletableFuture<T> result = new CompletableFuture<>();
        private final Predicate<T> predicate;
        private java.util.concurrent.Flow.Subscription subscription;

        Observation(Predicate<T> predicate) {
            this.predicate = predicate;
        }

        @Override
        public void onSubscribe(java.util.concurrent.Flow.Subscription subscription) {
            this.subscription = subscription;
            subscription.request(Long.MAX_VALUE);
        }

        @Override
        public void onNext(ZLinkObservedStatus<T> observed) {
            if (predicate.test(observed.status())) result.complete(observed.status());
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

    public record Join(String spotId) {}

    public record Probe(String value) {}

    public record Player(ZLinkActorContext context) implements ZLinkActor {
        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            joined.complete(completion);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class HeldSourceHandler implements AutoCloseable {
        private final CompletableFuture<Void> entered = scopeEntered;
        private final CompletableFuture<Void> release = scopeRelease;

        @Override
        public void close() {
            entered.complete(null);
            release.join();
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class PlayerState implements ZLinkActorRelocationAdapter<Player> {
        @Override
        public CompletionStage<byte[]> capture(
                Player actor, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[0]);
        }

        @Override
        public CompletionStage<Void> restore(
                Player actor, byte[] state, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public record BoundSession(ZLinkSessionContext context) implements ZLinkSession {
        @Override
        public CompletionStage<Void> onConnected() {
            connected.complete(context);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            relayed.completeExceptionally(
                    new IllegalStateException("physical Session disconnected"));
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public record Lobby(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<Player> {
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
            context.handlers().addHandler(LobbyJoinHandler.class);
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
        public void configure() {
            context.handlers().addHandler(RoomJoinHandler.class);
            context.handlers().addHandler(ProbeHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }
    }

    public static final class LobbyJoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Lobby, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Lobby spot, Player actor, ZLinkMessageContext context, Join request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class RoomJoinHandler
            implements ZLinkSpotActorRequestHandler<Room, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Room spot, Player actor, ZLinkMessageContext context, Join request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class ProbeHandler
            implements ZLinkSpotActorSendHandler<Room, Player, Probe> {
        @Override
        public CompletionStage<Void> handle(
                Room room, Player actor, ZLinkMessageContext context, Probe message) {
            relayed.complete(message.value());
            return CompletableFuture.completedFuture(null);
        }
    }
}
