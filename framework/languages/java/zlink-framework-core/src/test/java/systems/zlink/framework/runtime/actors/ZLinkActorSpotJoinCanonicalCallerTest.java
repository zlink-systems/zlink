package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.lang.reflect.Method;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserved;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkActorJoinAdmissionProfileCodec;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkActorJoinRelocationPort;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.spots.SpotHandles;
import systems.zlink.framework.spots.ZLinkSpotKind;

final class ZLinkActorSpotJoinCanonicalCallerTest {
    private static final RoutingId SOURCE = RoutingId.from("source-node");
    private static final RoutingId TARGET = RoutingId.from("target-node");
    private static final String ACTOR_ID = "actor-public-caller";
    private static final String TARGET_SPOT = "room-a";

    @Test
    void deferredPublicCallerStopsAfterAdmissionAndSubmitsCanonicalGoal()
        throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        ZLinkLocationOwnerToken sourceOwner = owner(locations, "source-owner");
        ZLinkLocationOwnerToken targetOwner = owner(locations, "target-owner");
        locations.updateMeshNode(
                descriptor(SOURCE, 11, sourceOwner, false),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture().join();
        locations.updateMeshNode(
                descriptor(TARGET, 19, targetOwner, true),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture().join();
        ZLinkAuthoritySnapshot actorAuthority = reserve(
            locations,
            sourceOwner,
            descriptorKey(SOURCE),
            11,
            ZLinkPlacementObjectKind.ACTOR,
            ZLinkAuthorityKeyCodec.actor(ACTOR_ID),
            "probe",
            new ZLinkActorAuthorityPayloadCodec().encode(
                ZLinkActorAuthorityPayloadCodec.State.READY,
                "probe", ACTOR_ID, "source-entry", 1, 1,
                sourceOwner.ownerId(), sourceOwner.leaseGeneration(),
                "mesh", SOURCE, 11),
            ZLinkPlacementCapacityBundle.actor(1));
        reserve(
            locations,
            targetOwner,
            descriptorKey(TARGET),
            19,
            ZLinkPlacementObjectKind.USER_SPOT,
            ZLinkAuthorityKeyCodec.spot(TARGET_SPOT),
            "room",
            new ZLinkServiceAuthorityPayloadCodec().encodeUser(
                ZLinkServiceAuthorityPayloadCodec.State.READY,
                "room", TARGET_SPOT,
                targetOwner.ownerId(), targetOwner.leaseGeneration(),
                "mesh", TARGET, 19),
            ZLinkPlacementCapacityBundle.spot(
                ZLinkPlacementObjectKind.USER_SPOT, "room", 1));

        ZLinkActorRuntime runtime = new ZLinkActorRuntime(
            sourceNode(actorAuthority.objectGeneration()),
            Map.of("probe", ProbeFactory.class),
            Duration.ofSeconds(1),
            new ZLinkJsonMessageSerializer(),
            ZLinkHandlerActivator.reflection());
        runtime.setMeshName("mesh");
        runtime.setRemoteAddressResolver(new FixedTargetResolver());
        List<ZLinkActorSpotRoutePackets.TransferRequest> requests =
            new CopyOnWriteArrayList<>();
        AtomicReference<ZLinkActorJoinRelocationPort.Goal> submittedGoal =
            new AtomicReference<>();
        ZLinkActorJoinRelocationPort port = new ZLinkActorJoinRelocationPort() {
            @Override
            public CompletionStage<Submission> relocate(
                Goal goal,
                Duration timeout) {
                submittedGoal.set(goal);
                return CompletableFuture.completedFuture(new Submission(
                    new ZLinkBackendActorRef(
                        TARGET, goal.sourceActor().actorId(),
                        goal.sourceActor().generation())));
            }

            @Override
            public void admit(Admission admission) {
                throw new AssertionError("source caller must not admit a target");
            }
        };
        runtime.setActorJoinRelocationPort(port);
        runtime.setActorJoinTransferTransport((address, parts, timeout, internal) -> {
            ZLinkActorSpotRoutePackets.TransferRequest decoded =
                ZLinkActorSpotRoutePackets.decodeTransferRequest(parts.get(1));
            requests.add(decoded);
            if (decoded.admission()) {
                return CompletableFuture.completedFuture(
                    ZLinkActorSpotRoutePackets.encodeAdmissionReply(
                        true, "target-spot", 1L, decoded.coreMembershipEpoch() + 1,
                        0L, Message.from(new byte[0])));
            }
            return CompletableFuture.failedFuture(
                new IllegalStateException("legacy commit must not be sent"));
        });

        ProbeActor actor = (ProbeActor) runtime.getOrCreateLocalActor(
                ACTOR_ID, ZLinkActor.class)
            .toCompletableFuture().join().orElseThrow();
        runtime.markJoinedEntrySpot(
            actor,
            new ZLinkBackendActorRef(
                SOURCE, ACTOR_ID, actorAuthority.objectGeneration()),
            SOURCE);
        Object publicCall = actor.context().joinSpot(TARGET_SPOT)
            .timeout(Duration.ofSeconds(2));
        Method execute = ZLinkActorSpotJoinCall.class.getDeclaredMethod(
            "execute", systems.zlink.framework.actors
                .ZLinkActorJoinOperationId.class);
        execute.setAccessible(true);
        var operationId = ZLinkActorSpotJoinCall.newOperationId();
        CompletionStage<?> operation = (CompletionStage<?>) execute.invoke(
            publicCall, operationId);
        operation.toCompletableFuture().join();

        assertEquals(1, requests.size(),
            "canonical Join has admission request/reply only; no commit reply");
        ZLinkActorSpotRoutePackets.TransferRequest admission = requests.getFirst();
        assertTrue(admission.admission());
        assertArrayEquals(new byte[0], admission.sessionRouteCommand44(),
            "pre-admission must not carry command44 before command42/43 seal");
        UUID relocationId = UUID.fromString(admission.transferId());
        ZLinkActorJoinRelocationPort.Goal goal = submittedGoal.get();
        assertEquals(relocationId, goal.relocationId());
        assertEquals(operationId, goal.operationId());
        assertNotEquals(
            relocationId,
            new UUID(operationId.high(), operationId.low()),
            "public OperationId is distinct from canonical RelocationId");
        runtime.closeAsync().toCompletableFuture().join();
    }

    @Test
    void hostDrainRetriesOneCanonicalOperationWithoutLegacyTransfer() {
        var locations = new ZLinkInMemoryLocationStore();
        ZLinkLocationOwnerToken sourceOwner = owner(locations, "source-owner");
        ZLinkLocationOwnerToken targetOwner = owner(locations, "target-owner");
        locations.updateMeshNode(
                descriptor(SOURCE, 11, sourceOwner, false),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture().join();
        locations.updateMeshNode(
                descriptor(TARGET, 19, targetOwner, true),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture().join();
        ZLinkAuthoritySnapshot actorAuthority = reserve(
            locations,
            sourceOwner,
            descriptorKey(SOURCE),
            11,
            ZLinkPlacementObjectKind.ACTOR,
            ZLinkAuthorityKeyCodec.actor(ACTOR_ID),
            "probe",
            new ZLinkActorAuthorityPayloadCodec().encode(
                ZLinkActorAuthorityPayloadCodec.State.READY,
                "probe", ACTOR_ID, "source-entry", 1, 1,
                sourceOwner.ownerId(), sourceOwner.leaseGeneration(),
                "mesh", SOURCE, 11),
            ZLinkPlacementCapacityBundle.actor(1));
        reserve(
            locations,
            targetOwner,
            descriptorKey(TARGET),
            19,
            ZLinkPlacementObjectKind.USER_SPOT,
            ZLinkAuthorityKeyCodec.spot(TARGET_SPOT),
            "room",
            new ZLinkServiceAuthorityPayloadCodec().encodeUser(
                ZLinkServiceAuthorityPayloadCodec.State.READY,
                "room", TARGET_SPOT,
                targetOwner.ownerId(), targetOwner.leaseGeneration(),
                "mesh", TARGET, 19),
            ZLinkPlacementCapacityBundle.spot(
                ZLinkPlacementObjectKind.USER_SPOT, "room", 1));

        ZLinkActorRuntime runtime = new ZLinkActorRuntime(
            sourceNode(actorAuthority.objectGeneration()),
            Map.of("probe", ProbeFactory.class),
            Duration.ofSeconds(1),
            new ZLinkJsonMessageSerializer(),
            ZLinkHandlerActivator.reflection());
        runtime.setMeshName("mesh");
        runtime.setRemoteAddressResolver(new FixedTargetResolver());
        List<ZLinkActorSpotRoutePackets.TransferRequest> requests =
            new CopyOnWriteArrayList<>();
        List<ZLinkActorJoinRelocationPort.Goal> goals =
            new CopyOnWriteArrayList<>();
        AtomicInteger admissionAttempts = new AtomicInteger();
        runtime.setActorJoinRelocationPort(new ZLinkActorJoinRelocationPort() {
            @Override
            public CompletionStage<Submission> relocate(
                Goal goal,
                Duration timeout) {
                goals.add(goal);
                return CompletableFuture.completedFuture(new Submission(
                    new ZLinkBackendActorRef(
                        TARGET, goal.sourceActor().actorId(),
                        goal.sourceActor().generation())));
            }

            @Override
            public void admit(Admission admission) {
                throw new AssertionError("source caller must not admit a target");
            }
        });
        runtime.setActorJoinTransferTransport((address, parts, timeout, internal) -> {
            ZLinkActorSpotRoutePackets.TransferRequest decoded =
                ZLinkActorSpotRoutePackets.decodeTransferRequest(parts.get(1));
            requests.add(decoded);
            if (!decoded.admission()) {
                return CompletableFuture.failedFuture(
                    new AssertionError("legacy transfer must not be sent"));
            }
            if (admissionAttempts.getAndIncrement() == 0) {
                return CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.BACKPRESSURED));
            }
            return CompletableFuture.completedFuture(
                ZLinkActorSpotRoutePackets.encodeAdmissionReply(
                    true, "target-spot", 1L, decoded.coreMembershipEpoch() + 1,
                    0L, Message.from(new byte[0])));
        });

        ProbeActor actor = (ProbeActor) runtime.getOrCreateLocalActor(
                ACTOR_ID, ZLinkActor.class)
            .toCompletableFuture().join().orElseThrow();
        runtime.markJoinedEntrySpot(
            actor,
            new ZLinkBackendActorRef(
                SOURCE, ACTOR_ID, actorAuthority.objectGeneration()),
            SOURCE);

        try {
            assertEquals(
                1,
                runtime.handoffActorsToEntrySpot("probe", "router", TARGET)
                    .toCompletableFuture().join());
            assertEquals(2, requests.size(),
                "one retry produces only admission requests");
            var firstOperation = ZLinkActorJoinAdmissionProfileCodec.decode(
                requests.get(0).adapterKey()).orElseThrow();
            var secondOperation = ZLinkActorJoinAdmissionProfileCodec.decode(
                requests.get(1).adapterKey()).orElseThrow();
            assertEquals(firstOperation, secondOperation,
                "host drain owns one operation identity across retry");
            assertEquals(1, goals.size(),
                "accepted admission submits exactly one canonical relocation goal");
            assertEquals(firstOperation, goals.getFirst().operationId());
        } finally {
            runtime.closeAsync().toCompletableFuture().join();
        }
    }

    private static ZLinkLocationOwnerToken owner(
        ZLinkInMemoryLocationStore locations,
        String ownerId) {
        return assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            locations.claimOwnerLease(ownerId, Duration.ofMinutes(5))
                .toCompletableFuture().join()).token();
    }

    private static ZLinkAuthoritySnapshot reserve(
        ZLinkInMemoryLocationStore locations,
        ZLinkLocationOwnerToken owner,
        ZLinkMeshNodeDescriptorKey descriptor,
        long descriptorGeneration,
        ZLinkPlacementObjectKind kind,
        String key,
        String stableType,
        byte[] payload,
        ZLinkPlacementCapacityBundle capacity) {
        var reservation = assertInstanceOf(
            ZLinkObjectReserved.class,
            locations.reserve(
                    new ZLinkObjectReservationRequest(
                        kind, key, stableType, "creation-root",
                        new byte[32], 32, descriptor, descriptorGeneration,
                        owner, payload, capacity),
                    () -> false)
                .toCompletableFuture().join()).reservation();
        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            locations.commit(reservation, payload, () -> false)
                .toCompletableFuture().join());
        return assertInstanceOf(
            ZLinkAuthoritySnapshot.class,
            locations.read(key, () -> false).toCompletableFuture().join());
    }

    private static ZLinkMeshNodeDescriptorKey descriptorKey(RoutingId rid) {
        return new ZLinkMeshNodeDescriptorKey("mesh", rid);
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        RoutingId rid,
        long generation,
        ZLinkLocationOwnerToken owner,
        boolean target) {
        List<ZLinkObjectCapability> capabilities = target
            ? List.of(
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.ACTOR,
                    "probe",
                    ZLinkObjectMaintenancePolicyKind.RECREATE,
                    false,
                    0),
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    "room",
                    ZLinkObjectMaintenancePolicyKind.RECREATE,
                    false,
                    0))
            : List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.ACTOR,
                "probe",
                ZLinkObjectMaintenancePolicyKind.RECREATE,
                false,
                0));
        return new ZLinkMeshNodeDescriptor(
            "mesh", rid, generation, 1,
            "tcp://127.0.0.1:" + (7_000 + generation),
            Map.of(), 1, capabilities,
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of((target ? "target" : "source") + "-entry"),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 8),
                new ZLinkCapacityUsage(0, 0, target ? 4 : 0),
                target
                    ? List.of(new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.USER_SPOT,
                        "room",
                        new ZLinkCapacityUsage(0, 0, 4)))
                    : List.of()),
            new ZLinkActivationConcurrency(0, 32),
            Optional.empty(),
            ZLinkFrameworkRuntimeState.SERVING,
            "security",
            owner.ownerId(), owner.leaseGeneration(), Instant.now());
    }

    private static ZLinkInternalSpotNode sourceNode(long generation) {
        ZLinkBackendSpot entry = (ZLinkBackendSpot) Proxy.newProxyInstance(
            ZLinkBackendSpot.class.getClassLoader(),
            new Class<?>[] {ZLinkBackendSpot.class},
            (proxy, method, args) -> defaultValue(method.getReturnType()));
        return (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, args) -> switch (method.getName()) {
                case "routingId" -> SOURCE;
                case "entrySpot" -> entry;
                case "createActor" -> {
                    ((Message) args[1]).close();
                    yield new ZLinkBackendActorRef(
                        SOURCE, (String) args[0], generation);
                }
                case "prepareActorTransfer" ->
                    throw new UnsupportedOperationException();
                case "actorMembershipEpoch" -> 1L;
                case "boundSessionRoute" -> Optional.empty();
                case "destroyActor" -> CompletableFuture.completedFuture(null);
                case "close" -> null;
                default -> defaultValue(method.getReturnType());
            });
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) return null;
        if (type == boolean.class) return false;
        if (type == byte.class) return (byte) 0;
        if (type == short.class) return (short) 0;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        if (type == float.class) return 0F;
        if (type == double.class) return 0D;
        if (type == char.class) return '\0';
        return null;
    }

    public static final class ProbeFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new ProbeActor(context));
        }
    }

    private record ProbeActor(ZLinkActorContext context) implements ZLinkActor {
        @Override
        public CompletionStage<Void> onJoinCompleted(
            ZLinkActorJoinCompletion completion) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class FixedTargetResolver
        implements SpotTransportAddressResolver, SpotHandleResolver {
        @Override
        public CompletionStage<Optional<SpotHandle>> resolveSpotHandle(
            String meshName,
            String spotId) {
            return resolveSpotHandle(spotId);
        }

        @Override
        public CompletionStage<Optional<SpotHandle>> resolveSpotHandle(
            String spotId) {
            return CompletableFuture.completedFuture(
                Optional.of(SpotHandles.create(spotId)));
        }

        @Override
        public CompletionStage<Optional<SpotTransportAddress>> resolve(
            String spotId) {
            return CompletableFuture.completedFuture(Optional.of(
                new SpotTransportAddress(
                    "router", TARGET, spotId, 1, 19, 1, 1,
                    ZLinkSpotKind.USER)));
        }
    }
}
