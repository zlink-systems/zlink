package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.relocation.ZLinkActorJoinRelocationPort;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;

import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkActorJoinStoreAdmissionTest {
    private static final String ACTOR_ID = "actor-a";
    private static final RoutingId NODE = RoutingId.from("node-a");

    @Test
    void localJoinAcceptedOnSpotLifecycleLaneCompletesAfterHostBeginsDraining() throws Exception {
        var options =
                new systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions();
        options.addLocationStore(
                new systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore());
        var node = options.addRouteMesh("drain-join");
        node.listen("inproc://join-drain-" + System.nanoTime()).setRoutingId(NODE);
        node.objects()
                .server()
                .addSpotFactory(
                        "drain-room", DrainRoom.class, factory -> factory.disableRelocation());
        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> release = new CompletableFuture<>();
        AtomicInteger callbacks = new AtomicInteger();
        try (var framework =
                systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess.start(
                        options,
                        new systems.zlink.framework.runtime.binding
                                .ZLinkJavaBackendAdapterFactory())) {
            framework
                    .spotManager()
                    .getOrCreate("drain-room-a", "drain-room")
                    .submit()
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            var field = framework.getClass().getDeclaredField("spots");
            field.setAccessible(true);
            ZLinkSpotRuntime host = (ZLinkSpotRuntime) field.get(framework);
            SpotActivation activation = host.spotLifecycle().spotActivationFor("drain-room-a");
            var jobsField = host.getClass().getDeclaredField("applicationJobQueue");
            jobsField.setAccessible(true);
            var jobs =
                    (systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue)
                            jobsField.get(host);
            var permits =
                    new java.util.ArrayList<
                            systems.zlink.framework.runtime.internal.dispatch
                                    .ZLinkApplicationJobQueue.Permit>();
            try {
                for (; ; ) {
                    var claim = jobs.acquire().toCompletableFuture();
                    if (!claim.isDone()) {
                        claim.cancel(false);
                        break;
                    }
                    permits.add(claim.join());
                }
                Thread.currentThread().interrupt();
                var interrupted =
                        assertThrows(
                                CompletionException.class,
                                () ->
                                        activation
                                                .admitLocalActorJoin(
                                                        () -> {
                                                            callbacks.incrementAndGet();
                                                            return CompletableFuture
                                                                    .completedFuture(
                                                                            ZLinkSpotActorJoinResult
                                                                                    .accept());
                                                        })
                                                .toCompletableFuture()
                                                .join());
                assertTrue(Thread.currentThread().isInterrupted());
                assertTrue(interrupted.getCause().getMessage().contains("interrupted"));
                assertEquals(0, callbacks.get());
            } finally {
                Thread.interrupted();
                permits.forEach(
                        systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue
                                        .Permit
                                ::close);
            }
            try {
                activation.context.enqueueJoinLifecycle(
                        () -> {
                            entered.complete(null);
                            return release;
                        });
                entered.get(5, TimeUnit.SECONDS);
                var accepted =
                        activation.admitLocalActorJoin(
                                () -> {
                                    callbacks.incrementAndGet();
                                    return CompletableFuture.completedFuture(
                                            ZLinkSpotActorJoinResult.accept());
                                });
                host.beginDrain().toCompletableFuture().get(5, TimeUnit.SECONDS);
                assertEquals(0, callbacks.get());
                org.junit.jupiter.api.Assertions.assertFalse(
                        accepted.toCompletableFuture().isDone());
                release.complete(null);
                assertTrue(accepted.toCompletableFuture().get(5, TimeUnit.SECONDS).accepted());
                assertEquals(1, callbacks.get());
                var rejected =
                        assertThrows(
                                CompletionException.class,
                                () ->
                                        activation
                                                .admitLocalActorJoin(
                                                        () -> {
                                                            callbacks.incrementAndGet();
                                                            return CompletableFuture
                                                                    .completedFuture(
                                                                            ZLinkSpotActorJoinResult
                                                                                    .accept());
                                                        })
                                                .toCompletableFuture()
                                                .join());
                assertEquals(
                        ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                        ((ZLinkFrameworkException) rejected.getCause()).kind());
                assertEquals(1, callbacks.get());
            } finally {
                release.complete(null);
            }
        }
    }

    public static final class DrainRoom
            implements systems.zlink.framework.spots.ZLinkSpot<ZLinkActor> {
        private final systems.zlink.framework.spots.ZLinkSpotContext context;

        public DrainRoom(systems.zlink.framework.spots.ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public systems.zlink.framework.spots.ZLinkSpotContext context() {
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
    }

    @Test
    void canonicalCommand28IngressUsesStoreResolvedTypeForCanonicalAdmission() throws Exception {
        ServiceWirePilotCodec.Fence actor =
                new ServiceWirePilotCodec.Fence(
                        ACTOR_ID,
                        7L,
                        NODE.toString().getBytes(StandardCharsets.UTF_8),
                        -9L,
                        2L,
                        3L);
        ServiceWirePilotCodec.Fence target =
                new ServiceWirePilotCodec.Fence(
                        "spot", 11L, NODE.toString().getBytes(StandardCharsets.UTF_8), -9L, 2L, 3L);
        for (List<byte[]> frames :
                List.of(
                        ServiceWirePilotCodec.encodeActorJoin28(
                                new ServiceWirePilotCodec.ActorJoin28(
                                        41L,
                                        actor,
                                        false,
                                        target,
                                        new ServiceWirePilotCodec.ApplicationPayloadEnvelopeV1(
                                                "JoinRequest",
                                                "application/json",
                                                "payload".getBytes(StandardCharsets.UTF_8)))),
                        ServiceWirePilotCodec.encodeActorJoin28(
                                new ServiceWirePilotCodec.ActorJoin28(
                                        42L, actor, true, target, null)))) {
            ServiceWirePilotCodec.ActorJoin28 decoded =
                    ServiceWirePilotCodec.decodeActorJoin28(frames);
            if (decoded.payload() == null) {
                assertNull(decoded.payload());
            } else {
                assertEquals("JoinRequest", decoded.payload().packetName());
                assertEquals("application/json", decoded.payload().contentType());
                assertArrayEquals(
                        "payload".getBytes(StandardCharsets.UTF_8), decoded.payload().payload());
            }
            AtomicInteger callbacks = new AtomicInteger();
            AtomicReference<ZLinkActorJoinRelocationPort.CanonicalAdmission> admitted =
                    new AtomicReference<>();
            var result =
                    admission(Map.of("canonical-type", Factory.class), admitted::set)
                            .prepareCanonicalRoutedActor(
                                    ZLinkSpotRuntime.canonicalActorJoinRequest(decoded, NODE),
                                    null,
                                    NODE,
                                    "spot",
                                    new Object(),
                                    decoded,
                                    decoded.payload() == null
                                            ? "application/octet-stream"
                                            : decoded.payload().contentType(),
                                    decoded.payload() == null
                                            ? new byte[0]
                                            : decoded.payload().payload(),
                                    ignored -> CompletableFuture.completedFuture(null),
                                    ignored -> {
                                        callbacks.incrementAndGet();
                                        return CompletableFuture.completedFuture(
                                                ZLinkSpotActorJoinResult.accept());
                                    })
                            .toCompletableFuture()
                            .join();
            assertEquals(true, result.accepted());
            assertEquals(1, callbacks.get());
            assertEquals(
                    "canonical-type",
                    admitted.get().actorType(),
                    "command-28 omits actorType; canonical admission must use the Store row");
        }
    }

    @Test
    void malformedCanonicalBodyIsRejectedAndCanonicalFenceFailureStaysTyped() {
        assertThrows(
                Exception.class,
                () ->
                        ServiceWirePilotCodec.decodeActorJoin28(
                                List.of(new byte[] {0x5a, 0x4d, 1, 28, 0})));
        ServiceWirePilotCodec.Fence forgedActor =
                new ServiceWirePilotCodec.Fence(
                        ACTOR_ID,
                        7L,
                        NODE.toString().getBytes(StandardCharsets.UTF_8),
                        -9L,
                        2L,
                        4L);
        ServiceWirePilotCodec.Fence target =
                new ServiceWirePilotCodec.Fence(
                        "spot", 11L, NODE.toString().getBytes(StandardCharsets.UTF_8), -9L, 2L, 3L);
        ServiceWirePilotCodec.ActorJoin28 forged =
                new ServiceWirePilotCodec.ActorJoin28(43L, forgedActor, false, target, null);
        CompletionException error =
                assertThrows(
                        CompletionException.class,
                        () ->
                                admission(Map.of("canonical-type", Factory.class))
                                        .prepareCanonicalRoutedActor(
                                                ZLinkSpotRuntime.canonicalActorJoinRequest(
                                                        forged, NODE),
                                                null,
                                                NODE,
                                                "spot",
                                                new Object(),
                                                forged,
                                                "application/octet-stream",
                                                new byte[0],
                                                ignored -> CompletableFuture.completedFuture(null),
                                                ignored ->
                                                        CompletableFuture.completedFuture(
                                                                ZLinkSpotActorJoinResult.accept()))
                                        .toCompletableFuture()
                                        .join());
        assertEquals(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                ((ZLinkFrameworkException) error.getCause()).kind());
    }

    private static ZLinkActorSpotAdmission admission(
            Map<String, Class<? extends ZLinkActorFactory>> factories) {
        return admission(factories, ignored -> {});
    }

    private static ZLinkActorSpotAdmission admission(
            Map<String, Class<? extends ZLinkActorFactory>> factories,
            java.util.function.Consumer<ZLinkActorJoinRelocationPort.CanonicalAdmission>
                    canonicalAdmission) {
        ZLinkInternalSpotNode node =
                (ZLinkInternalSpotNode)
                        Proxy.newProxyInstance(
                                ZLinkInternalSpotNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalSpotNode.class},
                                (proxy, method, arguments) ->
                                        "routingId".equals(method.getName()) ? NODE : null);
        ZLinkActorRuntime runtime =
                new ZLinkActorRuntime(
                        node, factories, Duration.ofSeconds(1), new ZLinkJsonMessageSerializer());
        runtime.setDirectJoinRelocationStores(store());
        runtime.setActorJoinRelocationPort(
                new ZLinkActorJoinRelocationPort() {
                    @Override
                    public CompletionStage<Submission> relocate(Goal goal, Duration timeout) {
                        return CompletableFuture.failedFuture(
                                new AssertionError("relocation is outside admission coverage"));
                    }

                    @Override
                    public void admit(Admission admission) {
                        throw new AssertionError("legacy admission is outside canonical coverage");
                    }

                    @Override
                    public void admitCanonical(CanonicalAdmission admission) {
                        canonicalAdmission.accept(admission);
                    }
                });
        ZLinkActorSpotAdmission admission = new ZLinkActorSpotAdmission();
        admission.attach(runtime, () -> false, null);
        return admission;
    }

    private static ZLinkLocationRepository store() {
        ZLinkAuthoritySnapshot row =
                new ZLinkAuthoritySnapshot(
                        "v1",
                        new byte[0],
                        7L,
                        2L,
                        "owner",
                        3L,
                        new ZLinkPlacementAllocation(
                                ZLinkPlacementAllocationState.ACTIVE,
                                ZLinkPlacementObjectKind.ACTOR,
                                "canonical-type",
                                new ZLinkMeshNodeDescriptorKey("mesh", NODE),
                                -9L,
                                ZLinkPlacementCapacityBundle.actor(1)),
                        Instant.now());
        return (ZLinkLocationRepository)
                Proxy.newProxyInstance(
                        ZLinkLocationRepository.class.getClassLoader(),
                        new Class<?>[] {ZLinkLocationRepository.class},
                        (proxy, method, arguments) ->
                                "read".equals(method.getName())
                                        ? CompletableFuture.completedFuture(row)
                                        : null);
    }

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public java.util.concurrent.CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.failedFuture(new AssertionError("not instantiated"));
        }
    }
}
