package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
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

final class ZLinkActorJoinStoreAdmissionTest {
    private static final String ACTOR_ID = "actor-a";
    private static final RoutingId NODE = RoutingId.from("node-a");

    @Test
    void storeResolvedTypeAdmitsBeforeApplicationCallback() {
        AtomicInteger callbacks = new AtomicInteger();
        var result = admission(Map.of("canonical-type", Factory.class))
            .prepareRoutedActor(request("canonical-type"), null, NODE, "spot", new Object(),
                actor -> CompletableFuture.completedFuture(null),
                ignored -> {
                    callbacks.incrementAndGet();
                    return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
                })
            .toCompletableFuture().join();

        assertEquals(true, result.accepted());
        assertEquals(1, callbacks.get());
    }

    @Test
    void forgedWireTypeIsTypeMismatchBeforeApplicationCallback() {
        AtomicInteger callbacks = new AtomicInteger();
        CompletionException error = assertThrows(CompletionException.class,
            () -> admission(Map.of("canonical-type", Factory.class))
                .prepareRoutedActor(request("forged-type"), null, NODE, "spot", new Object(),
                    actor -> CompletableFuture.completedFuture(null),
                    ignored -> {
                        callbacks.incrementAndGet();
                        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
                    })
                .toCompletableFuture().join());

        assertEquals(ZLinkFrameworkErrorKind.TYPE_MISMATCH,
            ((ZLinkFrameworkException) error.getCause()).kind());
        assertEquals(0, callbacks.get());
    }

    @Test
    void mismatchedFenceIsProtocolErrorBeforeApplicationCallback() {
        AtomicInteger callbacks = new AtomicInteger();
        CompletionException error = assertThrows(CompletionException.class,
            () -> admission(Map.of("canonical-type", Factory.class))
                .prepareRoutedActor(request("canonical-type", 8L),
                    null, NODE, "spot", new Object(),
                    actor -> CompletableFuture.completedFuture(null),
                    ignored -> {
                        callbacks.incrementAndGet();
                        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
                    })
                .toCompletableFuture().join());

        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ((ZLinkFrameworkException) error.getCause()).kind());
        assertEquals(0, callbacks.get());
    }

    @Test
    void missingFactoryIsTypedRejectionBeforeApplicationCallback() {
        AtomicInteger callbacks = new AtomicInteger();
        var result = admission(Map.of())
            .prepareRoutedActor(request("canonical-type"), null, NODE, "spot", new Object(),
                actor -> CompletableFuture.completedFuture(null),
                ignored -> {
                    callbacks.incrementAndGet();
                    return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
                })
            .toCompletableFuture().join();

        assertEquals(false, result.accepted());
        assertEquals(0, callbacks.get());
    }

    @Test
    void canonicalCommand28IngressUsesStoreResolvedTypeForCanonicalAdmission()
        throws Exception {
        ServiceWirePilotCodec.Fence actor = new ServiceWirePilotCodec.Fence(
            ACTOR_ID, 7L, NODE.toString().getBytes(StandardCharsets.UTF_8),
            -9L, 2L, 3L);
        ServiceWirePilotCodec.Fence target = new ServiceWirePilotCodec.Fence(
            "spot", 11L, NODE.toString().getBytes(StandardCharsets.UTF_8),
            -9L, 2L, 3L);
        for (List<byte[]> frames : List.of(
                ServiceWirePilotCodec.encodeActorJoin28(
                    new ServiceWirePilotCodec.ActorJoin28(41L, actor, false, target,
                        new ServiceWirePilotCodec.ApplicationPayloadEnvelopeV1(
                            "JoinRequest", "application/json",
                            "payload".getBytes(StandardCharsets.UTF_8)))),
                ServiceWirePilotCodec.encodeActorJoin28(
                    new ServiceWirePilotCodec.ActorJoin28(42L, actor, true, target, null)))) {
            ServiceWirePilotCodec.ActorJoin28 decoded =
                ServiceWirePilotCodec.decodeActorJoin28(frames);
            if (decoded.payload() == null) {
                assertNull(decoded.payload());
            } else {
                assertEquals("JoinRequest", decoded.payload().packetName());
                assertEquals("application/json", decoded.payload().contentType());
                assertArrayEquals("payload".getBytes(StandardCharsets.UTF_8),
                    decoded.payload().payload());
            }
            AtomicInteger callbacks = new AtomicInteger();
            AtomicReference<ZLinkActorJoinRelocationPort.CanonicalAdmission>
                admitted = new AtomicReference<>();
            var result = admission(
                    Map.of("canonical-type", Factory.class), admitted::set)
                .prepareCanonicalRoutedActor(
                    ZLinkSpotRuntime.canonicalActorJoinRequest(decoded, NODE),
                    null, NODE, "spot", new Object(),
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
                .toCompletableFuture().join();
            assertEquals(true, result.accepted());
            assertEquals(1, callbacks.get());
            assertEquals("canonical-type", admitted.get().actorType(),
                "command-28 omits actorType; canonical admission must use the Store row");
        }
    }

    @Test
    void malformedCanonicalBodyIsRejectedAndCanonicalFenceFailureStaysTyped() {
        assertThrows(Exception.class, () -> ServiceWirePilotCodec.decodeActorJoin28(
            List.of(new byte[] {0x5a, 0x4d, 1, 28, 0})));
        ServiceWirePilotCodec.Fence forgedActor = new ServiceWirePilotCodec.Fence(
            ACTOR_ID, 7L, NODE.toString().getBytes(StandardCharsets.UTF_8),
            -9L, 2L, 4L);
        ServiceWirePilotCodec.Fence target = new ServiceWirePilotCodec.Fence(
            "spot", 11L, NODE.toString().getBytes(StandardCharsets.UTF_8),
            -9L, 2L, 3L);
        CompletionException error = assertThrows(CompletionException.class,
            () -> admission(Map.of("canonical-type", Factory.class))
                .prepareCanonicalRoutedActor(
                    ZLinkSpotRuntime.canonicalActorJoinRequest(
                        new ServiceWirePilotCodec.ActorJoin28(
                            43L, forgedActor, false, target, null), NODE),
                    null, NODE, "spot", new Object(),
                    ignored -> CompletableFuture.completedFuture(null),
                    ignored -> CompletableFuture.completedFuture(
                        ZLinkSpotActorJoinResult.accept()))
                .toCompletableFuture().join());
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ((ZLinkFrameworkException) error.getCause()).kind());
    }

    private static ZLinkActorSpotAdmission admission(
        Map<String, Class<? extends ZLinkActorFactory>> factories) {
        return admission(factories, ignored -> { });
    }

    private static ZLinkActorSpotAdmission admission(
        Map<String, Class<? extends ZLinkActorFactory>> factories,
        java.util.function.Consumer<ZLinkActorJoinRelocationPort.CanonicalAdmission>
            canonicalAdmission) {
        ZLinkInternalSpotNode node = (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> "routingId".equals(method.getName())
                ? NODE : null);
        ZLinkActorRuntime runtime = new ZLinkActorRuntime(
            node, factories, Duration.ofSeconds(1), new ZLinkJsonMessageSerializer());
        runtime.setDirectJoinRelocationStores(store());
        runtime.setActorJoinRelocationPort(new ZLinkActorJoinRelocationPort() {
            @Override public CompletionStage<Submission> relocate(
                Goal goal,
                Duration timeout) {
                return CompletableFuture.failedFuture(
                    new AssertionError("relocation is outside admission coverage"));
            }

            @Override public void admit(Admission admission) {
                throw new AssertionError("legacy admission is outside canonical coverage");
            }

            @Override public void admitCanonical(CanonicalAdmission admission) {
                canonicalAdmission.accept(admission);
            }
        });
        ZLinkActorSpotAdmission admission = new ZLinkActorSpotAdmission();
        admission.attach(runtime, () -> false, null);
        return admission;
    }

    private static ZLinkActorSpotRoutePackets.TransferRequest request(String type) {
        return request(type, -9L);
    }

    private static ZLinkActorSpotRoutePackets.TransferRequest request(
        String type,
        long nodeGeneration) {
        return new ZLinkActorSpotRoutePackets.TransferRequest(
            "admission", "transfer-a", 1_000L, ACTOR_ID, type, NODE, 7L,
            RoutingId.from("entry"), "entry", "router", null, null, null,
            0, false, 0L, 0L, 0L, 0L, 0L, 0L, null, new byte[0],
            nodeGeneration, 2L, 3L);
    }

    private static ZLinkLocationRepository store() {
        ZLinkAuthoritySnapshot row = new ZLinkAuthoritySnapshot(
            "v1", new byte[0], 7L, 2L, "owner", 3L,
            new ZLinkPlacementAllocation(ZLinkPlacementAllocationState.ACTIVE,
                ZLinkPlacementObjectKind.ACTOR, "canonical-type",
                new ZLinkMeshNodeDescriptorKey("mesh", NODE), -9L,
                ZLinkPlacementCapacityBundle.actor(1)), Instant.now());
        return (ZLinkLocationRepository) Proxy.newProxyInstance(
            ZLinkLocationRepository.class.getClassLoader(),
            new Class<?>[] {ZLinkLocationRepository.class},
            (proxy, method, arguments) -> "read".equals(method.getName())
                ? CompletableFuture.completedFuture(row) : null);
    }

    public static final class Factory implements ZLinkActorFactory {
        @Override public java.util.concurrent.CompletionStage<ZLinkActor> create(
            ZLinkActorContext context) {
            return CompletableFuture.failedFuture(new AssertionError("not instantiated"));
        }
    }
}
