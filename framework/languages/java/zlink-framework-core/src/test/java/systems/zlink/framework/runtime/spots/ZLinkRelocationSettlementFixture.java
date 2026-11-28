package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotNull;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserved;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Shared relocation settlement harness: a source and a target state machine over one in-memory
 * Location Store.
 */
final class ZLinkRelocationSettlementFixture {
    static final java.time.Instant STAGE_DEADLINE = java.time.Instant.parse("2100-01-01T00:00:00Z");
    static final ZLinkStoreCancellation OPEN = () -> false;

    private ZLinkRelocationSettlementFixture() {}

    static final class Fixture {
        final AtomicReference<ZLinkAuthoritySnapshot> authorityRead = new AtomicReference<>();

        /** Every Location Store call the source and target machines made. */
        final AtomicInteger storeCalls = new AtomicInteger();

        /** While set, authority reads of the unit stay pending until it completes. */
        volatile CompletableFuture<Void> heldReads;

        /** While set, authority reads of the unit fail. */
        final AtomicBoolean failReads = new AtomicBoolean();

        final RoutingId sourceRid = RoutingId.from("source-node");
        final RoutingId targetRid = RoutingId.from("target-node");
        final ZLinkInMemoryLocationStore locations = new ZLinkInMemoryLocationStore();
        final AtomicInteger commits = new AtomicInteger();
        final AtomicBoolean connected = new AtomicBoolean(true);
        final java.util.concurrent.atomic.AtomicLong routeGeneration =
                new java.util.concurrent.atomic.AtomicLong(1);
        final AtomicBoolean silentlyDrop = new AtomicBoolean();
        final java.util.concurrent.BlockingQueue<Runnable> management =
                new java.util.concurrent.LinkedBlockingQueue<>();
        final AtomicBoolean dropCutover = new AtomicBoolean();
        final AtomicReference<java.util.function.Predicate<byte[]>> dropData =
                new AtomicReference<>(record -> false);
        final List<byte[]> data = new CopyOnWriteArrayList<>();
        final AtomicReference<byte[]> cutover = new AtomicReference<>();
        final AtomicReference<Runnable> cutoverWarning = new AtomicReference<>();
        final RecordingEndpoint endpoint = new RecordingEndpoint();
        final ZLinkLocationOwnerToken sourceOwner;
        final ZLinkLocationOwnerToken targetOwner;
        final String authorityKey;
        final ZLinkSpotRetireControl.StageRequest request;
        final ZLinkCanonicalRelocationStateMachine source;
        final ZLinkCanonicalRelocationStateMachine target;

        final AtomicBoolean ambiguousCommit = new AtomicBoolean();
        final CompletableFuture<Void> reconciliationEntered = new CompletableFuture<>();
        final CompletableFuture<Void> reconciliationRelease = new CompletableFuture<>();
        CompletableFuture<Void> stateDelivery = CompletableFuture.completedFuture(null);
        final CompletableFuture<Void> stateEntered = new CompletableFuture<>();
        final AtomicInteger failedReplies = new AtomicInteger();
        final AtomicLong targetAttemptGeneration = new AtomicLong();
        final List<Long> failedFailureCodes = new CopyOnWriteArrayList<>();
        CompletableFuture<Void> prepareCompletion = CompletableFuture.completedFuture(null);
        final CompletableFuture<Void> prepareEntered = new CompletableFuture<>();

        Fixture() {
            this(false, null);
        }

        Fixture(boolean manualManagement) {
            this(manualManagement, null);
        }

        Fixture(boolean manualManagement, ZLinkDispatchErrorReporter dispatchErrors) {
            String actorId = "actor-settlement";
            authorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
            sourceOwner = owner("source-owner");
            targetOwner = owner("target-owner");
            locations
                    .updateMeshNode(
                            descriptor(sourceRid, 11, sourceOwner, "source-entry"),
                            ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture()
                    .join();
            locations
                    .updateMeshNode(
                            descriptor(targetRid, 12, targetOwner, "target-entry"),
                            ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture()
                    .join();
            byte[] sourceAuthority =
                    new ZLinkActorAuthorityPayloadCodec()
                            .encode(
                                    ZLinkActorAuthorityPayloadCodec.State.READY,
                                    "actor-type",
                                    actorId,
                                    "source-entry",
                                    1,
                                    1,
                                    sourceOwner.ownerId(),
                                    sourceOwner.leaseGeneration(),
                                    "mesh",
                                    sourceRid,
                                    11);
            var reservation =
                    assertInstanceOf(
                                    ZLinkObjectReserved.class,
                                    locations
                                            .reserve(
                                                    new ZLinkObjectReservationRequest(
                                                            ZLinkPlacementObjectKind.ACTOR,
                                                            authorityKey,
                                                            "actor-type",
                                                            "creation-root",
                                                            new byte[32],
                                                            32,
                                                            new ZLinkMeshNodeDescriptorKey(
                                                                    "mesh", sourceRid),
                                                            11,
                                                            sourceOwner,
                                                            sourceAuthority,
                                                            ZLinkPlacementCapacityBundle.actor(1)),
                                                    OPEN)
                                            .toCompletableFuture()
                                            .join())
                            .reservation();
            assertEquals(
                    ZLinkObjectCommitResult.COMMITTED,
                    locations
                            .commit(reservation, sourceAuthority, OPEN)
                            .toCompletableFuture()
                            .join());
            ZLinkAuthoritySnapshot snapshot = authority();
            ZLinkLocationRepository observed =
                    (ZLinkLocationRepository)
                            Proxy.newProxyInstance(
                                    ZLinkLocationRepository.class.getClassLoader(),
                                    new Class<?>[] {ZLinkLocationRepository.class},
                                    (proxy, method, args) -> {
                                        storeCalls.incrementAndGet();
                                        if (method.getName().equals("read")
                                                && heldReads != null
                                                && authorityKey.equals(args[0]))
                                            return heldReads.thenCompose(
                                                    ignored -> locations.read(authorityKey, OPEN));
                                        if (method.getName().equals("read")
                                                && failReads.get()
                                                && authorityKey.equals(args[0]))
                                            return CompletableFuture.failedFuture(
                                                    new IllegalStateException("Store unavailable"));
                                        if (method.getName().equals("read")
                                                && authorityRead.get() != null
                                                && authorityKey.equals(args[0]))
                                            return CompletableFuture.completedFuture(
                                                    authorityRead.get());
                                        if (method.getName().equals("prepareAggregate")) {
                                            prepareEntered.complete(null);
                                            return prepareCompletion.thenCompose(
                                                    ignored -> {
                                                        try {
                                                            return (CompletionStage<?>)
                                                                    method.invoke(locations, args);
                                                        } catch (
                                                                ReflectiveOperationException
                                                                        failure) {
                                                            return CompletableFuture.failedFuture(
                                                                    failure);
                                                        }
                                                    });
                                        }
                                        if (method.getName().equals("commitAggregate")) {
                                            commits.incrementAndGet();
                                            if (ambiguousCommit.get()) {
                                                return ((CompletionStage<?>)
                                                                method.invoke(locations, args))
                                                        .thenCompose(
                                                                ignored ->
                                                                        CompletableFuture
                                                                                .failedFuture(
                                                                                        new IllegalStateException(
                                                                                                "CAS response"
                                                                                                        + " lost")));
                                            }
                                        }
                                        if (method.getName().equals("readAggregateProgress")
                                                && ambiguousCommit.get()
                                                && commits.get() != 0) {
                                            reconciliationEntered.complete(null);
                                            return reconciliationRelease.thenCompose(
                                                    ignored ->
                                                            locations.readAggregateProgress(
                                                                    (ZLinkAggregateFence) args[0],
                                                                    (ZLinkStoreCancellation)
                                                                            args[1]));
                                        }
                                        try {
                                            return method.invoke(locations, args);
                                        } catch (InvocationTargetException failure) {
                                            throw failure.getCause();
                                        }
                                    });
            byte[] targetAuthority =
                    new ZLinkActorAuthorityPayloadCodec()
                            .encode(
                                    ZLinkActorAuthorityPayloadCodec.State.READY,
                                    "actor-type",
                                    actorId,
                                    "target-entry",
                                    12,
                                    1,
                                    targetOwner.ownerId(),
                                    targetOwner.leaseGeneration(),
                                    "mesh",
                                    targetRid,
                                    12);
            var coordinator = new ZLinkAggregateRelocationCoordinator(observed);
            UUID relocationId = UUID.randomUUID();
            ZLinkSealedActorInventoryFixture.seal(
                    coordinator, relocationId, authorityKey, snapshot, targetAuthority, OPEN);
            byte[] root =
                    ZLinkCanonicalActorRelocationEnvelope.encode(
                            relocationId,
                            actorId,
                            snapshot.objectGeneration(),
                            snapshot.authorityOwnerGeneration(),
                            true,
                            new byte[] {1},
                            List.of());
            request =
                    new ZLinkSpotRetireControl.StageRequest(
                            new ZLinkSpotRetireControl.Fence(relocationId, 1),
                            sourceRid,
                            11,
                            sourceOwner.ownerId(),
                            sourceOwner.leaseGeneration(),
                            targetRid,
                            12,
                            targetOwner.ownerId(),
                            targetOwner.leaseGeneration(),
                            "mesh",
                            "target-entry",
                            "actor-type",
                            false,
                            true,
                            root,
                            List.of(
                                    new ZLinkSpotRetireControl.ParticipantFence(
                                            authorityKey,
                                            ZLinkPlacementObjectKind.ACTOR.value(),
                                            actorId,
                                            "actor-type",
                                            true,
                                            snapshot.objectGeneration(),
                                            snapshot.authorityOwnerGeneration())),
                            List.of());
            var options =
                    new ZLinkRelocationPayloadTransfer.Options(
                            262_144L,
                            16_777_216L,
                            0L,
                            Duration.ofMillis(50),
                            Duration.ofMillis(100));
            AtomicReference<ZLinkCanonicalRelocationStateMachine> sourceRef =
                    new AtomicReference<>();
            AtomicReference<ZLinkCanonicalRelocationStateMachine> targetRef =
                    new AtomicReference<>();
            source =
                    new ZLinkCanonicalRelocationStateMachine(
                            node(sourceRid, 11, targetRef, true),
                            "mesh",
                            "source-entry",
                            observed,
                            coordinator,
                            new RecordingEndpoint(),
                            (deadline, cleanup) -> {},
                            options,
                            System::nanoTime,
                            (delay, task) -> {
                                if (manualManagement) management.add(task);
                                else
                                    CompletableFuture.delayedExecutor(
                                                    delay.toMillis(), TimeUnit.MILLISECONDS)
                                            .execute(task);
                            });
            target =
                    new ZLinkCanonicalRelocationStateMachine(
                            node(targetRid, 12, sourceRef, false),
                            "mesh",
                            "target-entry",
                            observed,
                            coordinator,
                            endpoint,
                            (deadline, cleanup) -> {},
                            options,
                            System::nanoTime,
                            (delay, task) -> {
                                if (delay.equals(options.cutoverWaitTimeout())) {
                                    cutoverWarning.set(task);
                                } else if (manualManagement) {
                                    management.add(task);
                                } else {
                                    CompletableFuture.delayedExecutor(
                                                    delay.toMillis(), TimeUnit.MILLISECONDS)
                                            .execute(task);
                                }
                            },
                            dispatchErrors);
            sourceRef.set(source);
            targetRef.set(target);
        }

        void nextManagementTurn(CompletableFuture<?> settlement) throws Exception {
            // Store confirmation can finish settlement without scheduling another turn.
            settlement.whenComplete((ignored, failure) -> management.add(() -> {}));
            var turn = management.poll(3, TimeUnit.SECONDS);
            assertNotNull(turn, "source must schedule its authority observation");
            turn.run();
        }

        void stage() {
            startStage().join();
        }

        CompletableFuture<Void> startStage() {
            return source.stage(targetRid, request, Duration.ofSeconds(2), STAGE_DEADLINE)
                    .toCompletableFuture();
        }

        ZLinkFrameworkRuntime startTargetHost() throws Exception {
            var options = new DefaultZLinkFrameworkOptions();
            options.addLocationStore(new ZLinkInMemoryLocationStore());
            options.addRelocationStore(new InMemoryRelocationStore());
            var mesh =
                    options.addRouteMesh("shutdown-target")
                            .listen("inproc://shutdown-target-" + UUID.randomUUID());
            mesh.channelName("shutdown-target").server();
            mesh.objects().server();
            var runtime =
                    ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                            options,
                            new systems.zlink.framework.runtime.binding
                                    .ZLinkJavaBackendAdapterFactory(),
                            systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator
                                    .reflection(),
                            null,
                            new ZLinkFrameworkRuntimeTestAccess.Construction()
                                    .relocationStateMachines(List.of(target)));
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            runtime.setMessageFlowModeAsync(ZLinkMessageFlowLogMode.NORMAL).join();
            Object bridge = ZLinkFrameworkRuntimeTestAccess.spotRetire(runtime);
            assertNotNull(bridge);
            return runtime;
        }

        void relay(byte[]... records) {
            for (byte[] record : records) {
                source.relay(targetRid, request.fence(), record, Duration.ofSeconds(2))
                        .toCompletableFuture()
                        .handle((ignored, failure) -> null)
                        .join();
            }
        }

        void stageAndRelay(byte[]... records) {
            stage();
            relay(records);
        }

        void publish() {
            source.publish(targetRid, request.fence(), Duration.ofSeconds(2))
                    .toCompletableFuture()
                    .join();
        }

        void applyToTarget(int command, byte[] encoded) {
            target.apply(sourceRid, command, encoded)
                    .toCompletableFuture()
                    .handle((ignored, failure) -> null)
                    .join();
        }

        ZLinkAuthoritySnapshot authority() {
            return assertInstanceOf(
                    ZLinkAuthoritySnapshot.class,
                    locations.read(authorityKey, OPEN).toCompletableFuture().join());
        }

        private ZLinkLocationOwnerToken owner(String ownerId) {
            return assertInstanceOf(
                            ZLinkOwnerLeaseClaimed.class,
                            locations
                                    .claimOwnerLease(ownerId, Duration.ofMinutes(5))
                                    .toCompletableFuture()
                                    .join())
                    .token();
        }

        private ZLinkInternalMeshNode node(
                RoutingId localRid,
                long generation,
                AtomicReference<ZLinkCanonicalRelocationStateMachine> peer,
                boolean sourceSide) {
            MeshNodeStatus status =
                    new MeshNodeStatus(
                            MeshNodeState.READY,
                            localRid,
                            "mesh",
                            "",
                            generation,
                            1,
                            0,
                            1,
                            1,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0);
            return (ZLinkInternalMeshNode)
                    Proxy.newProxyInstance(
                            ZLinkInternalMeshNode.class.getClassLoader(),
                            new Class<?>[] {ZLinkInternalMeshNode.class},
                            (proxy, method, args) ->
                                    switch (method.getName()) {
                                        case "status" -> status;
                                        case "isPeerTransportConnected" -> connected.get();
                                        case "observePeerRoute" ->
                                                new ZLinkInternalMeshNode.PeerRouteObservation(
                                                        routeGeneration.get(), connected.get());
                                        case "onStateChanged", "onPeerStateChanged" -> {
                                            ((Runnable)
                                                            args[
                                                                    method.getName()
                                                                                    .equals(
                                                                                            "onPeerStateChanged")
                                                                            ? 1
                                                                            : 0])
                                                    .run();
                                            yield (AutoCloseable) () -> {};
                                        }
                                        case "requestCanonicalRelocationPrepare" -> {
                                            byte[] encoded = ((byte[]) args[1]).clone();
                                            targetAttemptGeneration.set(
                                                    ZLinkCanonicalRelocationProtocol.decodePrepare(
                                                                    encoded)
                                                            .targetAttemptGeneration());
                                            var reply =
                                                    peer.get()
                                                            .apply(
                                                                    localRid,
                                                                    1L,
                                                                    Byte.toUnsignedInt(encoded[3]),
                                                                    encoded);
                                            ((CompletableFuture<Void>) args[3]).complete(null);
                                            yield reply.thenApply(
                                                    encodedReply -> {
                                                        if (Byte.toUnsignedInt(encodedReply[3])
                                                                == ServiceWireConstants
                                                                        .COMMAND_RELOCATION_FAILED) {
                                                            failedReplies.incrementAndGet();
                                                            failedFailureCodes.add(
                                                                    ZLinkCanonicalRelocationProtocol
                                                                            .decodeFailed(
                                                                                    encodedReply)
                                                                            .failureCode());
                                                        }
                                                        return encodedReply;
                                                    });
                                        }
                                        case "sendCanonicalRelocationControl" -> {
                                            byte[] encoded = ((byte[]) args[1]).clone();
                                            int command = Byte.toUnsignedInt(encoded[3]);
                                            if (!sourceSide
                                                    && command
                                                            == ServiceWireConstants
                                                                    .COMMAND_RELOCATION_FAILED) {
                                                failedReplies.incrementAndGet();
                                            }
                                            if (sourceSide
                                                    && command
                                                            == ServiceWireConstants
                                                                    .COMMAND_RELOCATION_STATE) {
                                                stateEntered.complete(null);
                                                yield stateDelivery.thenCompose(
                                                        ignored ->
                                                                peer.get()
                                                                        .apply(
                                                                                localRid, command,
                                                                                encoded));
                                            }
                                            if (sourceSide
                                                    && command
                                                            == ServiceWireConstants
                                                                    .COMMAND_RELOCATION_DATA) {
                                                data.add(encoded);
                                                byte[] record =
                                                        ZLinkCanonicalRelocationProtocol.decodeData(
                                                                        encoded)
                                                                .frozenRecord();
                                                if (dropData.get().test(record)) {
                                                    if (silentlyDrop.get())
                                                        yield CompletableFuture.completedFuture(
                                                                null);
                                                    yield CompletableFuture.failedFuture(
                                                            new IllegalStateException(
                                                                    "DATA lost with the"
                                                                            + " connection"));
                                                }
                                            }
                                            if (sourceSide
                                                    && command
                                                            == ServiceWireConstants
                                                                    .COMMAND_RELOCATION_CUTOVER) {
                                                cutover.set(encoded);
                                                if (dropCutover.get()) {
                                                    if (silentlyDrop.get())
                                                        yield CompletableFuture.completedFuture(
                                                                null);
                                                    yield CompletableFuture.failedFuture(
                                                            new IllegalStateException(
                                                                    "CUTOVER lost with the"
                                                                            + " connection"));
                                                }
                                            }
                                            yield peer.get().apply(localRid, command, encoded);
                                        }
                                        default ->
                                                throw new UnsupportedOperationException(
                                                        method.getName());
                                    });
        }

        private static ZLinkMeshNodeDescriptor descriptor(
                RoutingId rid, long generation, ZLinkLocationOwnerToken owner, String entrySpotId) {
            return new ZLinkMeshNodeDescriptor(
                    "mesh",
                    rid,
                    generation,
                    1,
                    "tcp://127.0.0.1:" + (7_000 + generation),
                    Map.of(),
                    1,
                    List.of(
                            new ZLinkObjectCapability(
                                    ZLinkPlacementObjectKind.ACTOR,
                                    "actor-type",
                                    ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                                    true,
                                    0)),
                    ZLinkMeshNodeObjectRole.SERVER,
                    Optional.of(entrySpotId),
                    100,
                    new ZLinkPlacementCapacity(
                            new ZLinkCapacityUsage(0, 0, 8),
                            new ZLinkCapacityUsage(0, 0, 0),
                            List.of()),
                    new ZLinkActivationConcurrency(0, 32),
                    Optional.empty(),
                    ZLinkFrameworkRuntimeState.SERVING,
                    "security",
                    owner.ownerId(),
                    owner.leaseGeneration(),
                    Instant.now());
        }
    }

    static final class RecordingEndpoint implements ZLinkSpotRetireControl.TargetEndpoint {
        final AtomicInteger aborted = new AtomicInteger();
        final AtomicInteger abortedBeforeInstalled = new AtomicInteger();
        final CompletableFuture<Void> stageEntered = new CompletableFuture<>();
        CompletableFuture<Void> stageCompletion = CompletableFuture.completedFuture(null);
        boolean installed;
        final AtomicInteger published = new AtomicInteger();
        final CompletableFuture<Void> abortedEvent = new CompletableFuture<>();
        final CompletableFuture<Void> publishedEvent = new CompletableFuture<>();
        CompletableFuture<Void> relayCompletion = CompletableFuture.completedFuture(null);
        private final List<byte[]> relayed = new CopyOnWriteArrayList<>();

        List<byte[]> relayed() {
            return new ArrayList<>(relayed);
        }

        @Override
        public CompletionStage<Void> stage(ZLinkSpotRetireControl.StageRequest request) {
            stageEntered.complete(null);
            return stageCompletion.thenRun(() -> installed = true);
        }

        @Override
        public CompletionStage<Void> stageRelayedRecord(
                ZLinkSpotRetireControl.StageRequest request, byte[] frozenRecord) {
            relayed.add(frozenRecord.clone());
            return relayCompletion;
        }

        @Override
        public CompletionStage<Void> publish(ZLinkSpotRetireControl.StageRequest request) {
            published.incrementAndGet();
            publishedEvent.complete(null);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> abort(ZLinkSpotRetireControl.StageRequest request) {
            if (!installed) {
                abortedBeforeInstalled.incrementAndGet();
                return CompletableFuture.failedFuture(
                        new IllegalStateException("target stage is unavailable"));
            }
            installed = false;
            aborted.incrementAndGet();
            abortedEvent.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }
}
