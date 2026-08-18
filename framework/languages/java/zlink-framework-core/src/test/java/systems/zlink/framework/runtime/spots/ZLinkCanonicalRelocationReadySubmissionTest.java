package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityGenerationTransition;
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
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

final class ZLinkCanonicalRelocationReadySubmissionTest {
    private static final ZLinkStoreCancellation OPEN = () -> false;

    @Test
    void readySubmissionFailureRollsBackAndExactPrepareRetryRestages()
        throws Exception {
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId targetRid = RoutingId.from("target-node");
        String actorId = "actor-ready-race";
        String authorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        ZLinkInMemoryLocationStore locations = new ZLinkInMemoryLocationStore();
        ZLinkLocationOwnerToken sourceOwner = owner(locations, "source-owner");
        ZLinkLocationOwnerToken targetOwner = owner(locations, "target-owner");
        locations.updateMeshNode(
                descriptor(sourceRid, 11, sourceOwner, "source-entry"),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture().join();
        locations.updateMeshNode(
                descriptor(targetRid, 12, targetOwner, "target-entry"),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture().join();
        byte[] sourceAuthority = actorAuthority(
            actorId, sourceRid, sourceOwner).payload();
        var reservation = assertInstanceOf(
            ZLinkObjectReserved.class,
            locations.reserve(
                    new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.ACTOR,
                        authorityKey,
                        "actor-type",
                        "creation-root",
                        new byte[32],
                        32,
                        new ZLinkMeshNodeDescriptorKey("mesh", sourceRid),
                        11,
                        sourceOwner,
                        sourceAuthority,
                        ZLinkPlacementCapacityBundle.actor(1)),
                    OPEN)
                .toCompletableFuture().join()).reservation();
        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            locations.commit(reservation, sourceAuthority, OPEN)
                .toCompletableFuture().join());
        ZLinkAuthoritySnapshot sourceSnapshot = assertInstanceOf(
            ZLinkAuthoritySnapshot.class,
            locations.read(authorityKey, OPEN).toCompletableFuture().join());
        byte[] targetAuthority = new ZLinkActorAuthorityPayloadCodec().encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "actor-type", actorId, "target-entry",
            12, 1, targetOwner.ownerId(), targetOwner.leaseGeneration(),
            "mesh", targetRid, 12);

        AtomicInteger commits = new AtomicInteger();
        AtomicLong firstCommitNanos = new AtomicLong();
        AtomicInteger aggregateAborts = new AtomicInteger();
        ZLinkLocationRepository observedLocations =
            (ZLinkLocationRepository) Proxy.newProxyInstance(
                ZLinkLocationRepository.class.getClassLoader(),
                new Class<?>[] {ZLinkLocationRepository.class},
                (proxy, method, args) -> {
                    if (method.getName().equals("commitAggregate")) {
                        commits.incrementAndGet();
                        firstCommitNanos.compareAndSet(0L, System.nanoTime());
                    } else if (method.getName().equals("abortAggregate")) {
                        aggregateAborts.incrementAndGet();
                    }
                    try {
                        return method.invoke(locations, args);
                    } catch (InvocationTargetException failure) {
                        throw failure.getCause();
                    }
                });
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            observedLocations);
        UUID relocationId = UUID.randomUUID();
        byte[] root = ZLinkCanonicalActorRelocationEnvelope.encode(
            relocationId,
            actorId,
            sourceSnapshot.objectGeneration(),
            sourceSnapshot.authorityOwnerGeneration(),
            true,
            new byte[] {1},
            List.of());
        var request = new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(relocationId, 1),
            sourceRid, 11, sourceOwner.ownerId(), sourceOwner.leaseGeneration(),
            targetRid, 12, targetOwner.ownerId(), targetOwner.leaseGeneration(),
            "mesh", "target-entry", "actor-type", false, true,
            root,
            List.of(new ZLinkSpotRetireControl.ParticipantFence(
                authorityKey,
                ZLinkPlacementObjectKind.ACTOR.value(),
                actorId,
                "actor-type",
                true,
                sourceSnapshot.objectGeneration(),
                sourceSnapshot.authorityOwnerGeneration())),
            List.of());

        var timedOutSource = new ZLinkCanonicalRelocationStateMachine(
            droppingNode(sourceRid, 11),
            "mesh", "source-entry", observedLocations, coordinator,
            new CountingEndpoint());
        ExecutionException readyTimeout = assertThrows(
            ExecutionException.class,
            () -> timedOutSource.stage(
                    targetRid, request, Duration.ofMillis(25))
                .toCompletableFuture().get(250, TimeUnit.MILLISECONDS));
        assertInstanceOf(
            java.util.concurrent.TimeoutException.class,
            readyTimeout.getCause());
        assertEquals(0, attemptCount(timedOutSource, "sources"),
            "READY timeout must release the canonical source attempt");

        AtomicReference<byte[]> overlappingPrepare = new AtomicReference<>();
        var overlappingSource = new ZLinkCanonicalRelocationStateMachine(
            droppingNode(sourceRid, 11, overlappingPrepare),
            "mesh", "source-entry", observedLocations, coordinator,
            new CountingEndpoint());
        CompletableFuture<Void> ownerWaiter = overlappingSource.stage(
                targetRid, request, Duration.ofMillis(250))
            .toCompletableFuture();
        CompletableFuture<Void> shortWaiter = overlappingSource.stage(
                targetRid, request, Duration.ofMillis(25))
            .toCompletableFuture();
        assertThrows(
            ExecutionException.class,
            () -> shortWaiter.get(200, TimeUnit.MILLISECONDS));
        assertEquals(1, attemptCount(overlappingSource, "sources"),
            "a duplicate waiter timeout must not remove the owner attempt");
        var overlapping = ZLinkCanonicalRelocationProtocol.decodePrepare(
            overlappingPrepare.get());
        overlappingSource.apply(
                targetRid,
                ServiceWireConstants.COMMAND_RELOCATION_READY,
                ZLinkCanonicalRelocationProtocol.encodeReady(
                    new ZLinkCanonicalRelocationProtocol.Ready(
                        overlapping.id(),
                        overlapping.targetAttemptGeneration(),
                        overlapping.coordinator(),
                        overlapping.target(),
                        overlapping.object(),
                        ZLinkCanonicalRelocationProtocol.TARGET)))
            .toCompletableFuture().join();
        ownerWaiter.get(200, TimeUnit.MILLISECONDS);
        overlappingSource.publish(
                targetRid, request.fence(), Duration.ofMillis(100))
            .toCompletableFuture().join();
        assertEquals(0, attemptCount(overlappingSource, "sources"));

        AtomicReference<byte[]> failedPrepare = new AtomicReference<>();
        var failedSource = new ZLinkCanonicalRelocationStateMachine(
            droppingNode(sourceRid, 11, failedPrepare),
            "mesh", "source-entry", observedLocations, coordinator,
            new CountingEndpoint());
        CompletableFuture<Void> failedStage = failedSource.stage(
            targetRid, request, Duration.ofMillis(500)).toCompletableFuture();
        var failed = ZLinkCanonicalRelocationProtocol.decodePrepare(
            failedPrepare.get());
        failedSource.apply(
                targetRid,
                ServiceWireConstants.COMMAND_RELOCATION_FAILED,
                ZLinkCanonicalRelocationProtocol.encodeFailed(
                    new ZLinkCanonicalRelocationProtocol.Failed(
                        failed.id(),
                        failed.targetAttemptGeneration(),
                        failed.coordinator(),
                        failed.target(),
                        failed.object(),
                        ZLinkCanonicalRelocationProtocol.TARGET,
                        ServiceWireConstants.FRAMEWORK_ERROR_RELOCATION_DATA_LOST)))
            .toCompletableFuture().join();
        assertTrue(failedStage.isCompletedExceptionally(),
            "an exact target failure reply must complete the source waiter");
        ExecutionException stageFailure = assertThrows(
            ExecutionException.class,
            () -> failedStage.get(500, TimeUnit.MILLISECONDS),
            "a pre-prepare target stage failure must terminate at the source");
        assertInstanceOf(IllegalStateException.class, stageFailure.getCause());

        AtomicReference<ZLinkCanonicalRelocationStateMachine> source =
            new AtomicReference<>();
        AtomicReference<ZLinkCanonicalRelocationStateMachine> target =
            new AtomicReference<>();
        AtomicBoolean rejectFirstReady = new AtomicBoolean(true);
        AtomicBoolean rejectFirstCutover = new AtomicBoolean(true);
        AtomicReference<byte[]> rejectedCutover = new AtomicReference<>();
        AtomicReference<byte[]> submittedPrepare = new AtomicReference<>();
        AtomicLong readySubmittedNanos = new AtomicLong();
        List<ScheduledExpiry> retainedExpiry =
            new CopyOnWriteArrayList<>();
        CountingEndpoint targetEndpoint = new CountingEndpoint();
        source.set(new ZLinkCanonicalRelocationStateMachine(
            node(sourceRid, 11, target, rejectFirstReady,
                rejectFirstCutover, rejectedCutover, submittedPrepare,
                readySubmittedNanos),
            "mesh", "source-entry", observedLocations, coordinator,
            new CountingEndpoint()));
        target.set(new ZLinkCanonicalRelocationStateMachine(
            node(targetRid, 12, source, rejectFirstReady,
                rejectFirstCutover, rejectedCutover, submittedPrepare,
                readySubmittedNanos),
            "mesh", "target-entry", observedLocations, coordinator,
            targetEndpoint,
            (deadline, cleanup) -> retainedExpiry.add(
                new ScheduledExpiry(deadline, cleanup))));

        assertThrows(CompletionException.class, () -> source.get().stage(
                targetRid, request, Duration.ofMillis(100))
            .toCompletableFuture().join());
        Thread.sleep(1_100L);
        assertEquals(0, commits.get(),
            "a failed READY submission must not arm the 1000 ms fallback");
        assertEquals(1, targetEndpoint.aborted.get(),
            "the hidden Restore is erased when READY cannot be submitted");
        assertEquals(0, aggregateAborts.get(),
            "the exact immutable Prepare fence remains retryable rather than terminal ABORTED");
        assertEquals(1, attemptCount(target.get(), "retryPrepared"));
        assertEquals(1, retainedExpiry.size(),
            "READY failure must bind retry retention to Restore expiry");

        var changed = new ZLinkSpotRetireControl.StageRequest(
            request.fence(),
            request.sourceNodeRid(),
            request.sourceNodeGeneration(),
            request.sourceOwnerId(),
            request.sourceOwnerLeaseGeneration(),
            request.targetNodeRid(),
            request.targetNodeGeneration(),
            request.targetOwnerId() + "-different",
            request.targetOwnerLeaseGeneration(),
            request.meshName(),
            request.spotId(),
            request.stableType(),
            request.instanceSpot(),
            request.restoreSpotSnapshot(),
            request.relocationPayload(),
            request.participants(),
            request.sessionRoutes());
        assertThrows(CompletionException.class, () -> source.get().stage(
                targetRid, changed, Duration.ofMillis(100))
            .toCompletableFuture().join(),
            "a non-exact retry on the same fence remains a conflict");
        assertEquals(1, targetEndpoint.staged.get());

        Throwable retryFailure = null;
        try {
            source.get().stage(targetRid, request, Duration.ofMillis(250))
                .toCompletableFuture().join();
        } catch (CompletionException failure) {
            retryFailure = failure.getCause();
        }
        assertNull(retryFailure,
            "the exact same Prepare retry must restage after READY send failure");
        assertEquals(2, targetEndpoint.staged.get());
        assertEquals(0, attemptCount(target.get(), "retryPrepared"));
        assertThrows(CompletionException.class, () -> source.get().publish(
                targetRid, request.fence(), Duration.ofSeconds(1))
            .toCompletableFuture().join());
        assertEquals(0, attemptCount(source.get(), "sources"),
            "a failed CUTOVER transport submission must release the source attempt");
        Thread.sleep(1_100L);
        assertEquals(1, commits.get());
        assertEquals(1, targetEndpoint.published.get());
        assertTrue(firstCommitNanos.get() - readySubmittedNanos.get()
                >= TimeUnit.SECONDS.toNanos(1),
            "target fallback must not run before 1000 ms after READY submission");
        assertEquals(0, attemptCount(target.get(), "targets"),
            "a terminal target publication must release its active attempt");
        assertEquals(1, attemptCount(target.get(), "terminalTargets"),
            "the exact terminal identity remains as a bounded tombstone");
        int stagedBeforeTerminalDuplicate = targetEndpoint.staged.get();
        target.get().apply(
                sourceRid,
                ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
                submittedPrepare.get())
            .toCompletableFuture().join();
        assertEquals(stagedBeforeTerminalDuplicate, targetEndpoint.staged.get(),
            "terminal duplicate PREPARE must not reuse the released target stage");
        var prepared = ZLinkCanonicalRelocationProtocol.decodePrepare(
            submittedPrepare.get());
        byte[] terminalData = ZLinkCanonicalRelocationProtocol.encodeData(
            new ZLinkCanonicalRelocationProtocol.Data(
                prepared.id(),
                prepared.targetAttemptGeneration(),
                prepared.coordinator(),
                ZLinkCanonicalRelocationProtocol.SOURCE,
                prepared.object(),
                new byte[] {1}));
        assertThrows(CompletionException.class, () -> target.get().apply(
                sourceRid,
                ServiceWireConstants.COMMAND_RELOCATION_DATA,
                terminalData)
            .toCompletableFuture().join(),
            "terminal DATA must not enter a released target stage");
        byte[] cutover = rejectedCutover.get();
        target.get().apply(
                sourceRid,
                ServiceWireConstants.COMMAND_RELOCATION_CUTOVER,
                cutover)
            .toCompletableFuture().join();
        target.get().apply(
                sourceRid,
                ServiceWireConstants.COMMAND_RELOCATION_CUTOVER,
                cutover)
            .toCompletableFuture().join();
        assertEquals(1, commits.get(),
            "late and duplicate CUTOVER after fallback mutate nothing");
        assertEquals(2, retainedExpiry.size(),
            "the target tombstone must share the Restore expiry boundary");
        retainedExpiry.forEach(value -> value.cleanup().run());
        assertEquals(0, attemptCount(target.get(), "targets"));
        assertEquals(0, attemptCount(target.get(), "retryPrepared"));
        assertEquals(0, attemptCount(target.get(), "terminalTargets"));
        assertThrows(CompletionException.class, () -> target.get().apply(
                sourceRid,
                ServiceWireConstants.COMMAND_RELOCATION_CUTOVER,
                cutover)
            .toCompletableFuture().join(),
            "an expired tombstone must not turn unknown CUTOVER into success");
    }

    private static ZLinkLocationOwnerToken owner(
        ZLinkInMemoryLocationStore locations,
        String ownerId) {
        return assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            locations.claimOwnerLease(ownerId, Duration.ofMinutes(5))
                .toCompletableFuture().join()).token();
    }

    private static ZLinkAuthoritySnapshot actorAuthority(
        String actorId,
        RoutingId sourceRid,
        ZLinkLocationOwnerToken sourceOwner) {
        byte[] payload = new ZLinkActorAuthorityPayloadCodec().encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "actor-type", actorId, "source-entry",
            1, 1, sourceOwner.ownerId(), sourceOwner.leaseGeneration(),
            "mesh", sourceRid, 11);
        return new ZLinkAuthoritySnapshot(
            "store-1", payload, 3, 5,
            sourceOwner.ownerId(), sourceOwner.leaseGeneration(),
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.ACTIVE,
                ZLinkPlacementObjectKind.ACTOR,
                "actor-type",
                new ZLinkMeshNodeDescriptorKey("mesh", sourceRid),
                11,
                ZLinkPlacementCapacityBundle.actor(1)),
            Instant.now());
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        RoutingId rid,
        long generation,
        ZLinkLocationOwnerToken owner,
        String entrySpotId) {
        return new ZLinkMeshNodeDescriptor(
            "mesh", rid, generation, 1,
            "tcp://127.0.0.1:" + (7_000 + generation),
            Map.of(),
            1,
            List.of(new ZLinkObjectCapability(
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

    private static ZLinkInternalMeshNode node(
        RoutingId localRid,
        long generation,
        AtomicReference<ZLinkCanonicalRelocationStateMachine> peer,
        AtomicBoolean rejectFirstReady,
        AtomicBoolean rejectFirstCutover,
        AtomicReference<byte[]> rejectedCutover,
        AtomicReference<byte[]> submittedPrepare,
        AtomicLong readySubmittedNanos) {
        MeshNodeStatus status = new MeshNodeStatus(
            MeshNodeState.READY, localRid, "mesh", "", generation,
            1, 0, 1, 1, 0, 0, 0, 0, 0, 0);
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkInternalMeshNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, args) -> switch (method.getName()) {
                case "status" -> status;
                case "sendCanonicalRelocationControl" -> {
                    byte[] encoded = (byte[]) args[1];
                    int command = Byte.toUnsignedInt(encoded[3]);
                    if (command
                        == ServiceWireConstants.COMMAND_RELOCATION_PREPARE) {
                        submittedPrepare.set(encoded.clone());
                    }
                    if (command == ServiceWireConstants.COMMAND_RELOCATION_READY
                        && rejectFirstReady.compareAndSet(true, false)) {
                        yield CompletableFuture.failedFuture(
                            new IllegalStateException("READY transport rejected"));
                    }
                    if (command
                            == ServiceWireConstants.COMMAND_RELOCATION_CUTOVER
                        && rejectFirstCutover.compareAndSet(true, false)) {
                        rejectedCutover.set(encoded.clone());
                        yield CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "CUTOVER transport rejected"));
                    }
                    CompletionStage<Void> delivered = peer.get().apply(
                        localRid, command, encoded);
                    if (command == ServiceWireConstants.COMMAND_RELOCATION_READY) {
                        delivered = delivered.thenRun(() ->
                            readySubmittedNanos.compareAndSet(
                                0L, System.nanoTime()));
                    }
                    yield delivered;
                }
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
    }

    private static ZLinkInternalMeshNode droppingNode(
        RoutingId localRid,
        long generation) {
        return droppingNode(localRid, generation, new AtomicReference<>());
    }

    private static ZLinkInternalMeshNode droppingNode(
        RoutingId localRid,
        long generation,
        AtomicReference<byte[]> submittedPrepare) {
        MeshNodeStatus status = new MeshNodeStatus(
            MeshNodeState.READY, localRid, "mesh", "", generation,
            1, 0, 1, 1, 0, 0, 0, 0, 0, 0);
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkInternalMeshNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, args) -> switch (method.getName()) {
                case "status" -> status;
                case "sendCanonicalRelocationControl" -> {
                    byte[] encoded = (byte[]) args[1];
                    if (Byte.toUnsignedInt(encoded[3])
                        == ServiceWireConstants.COMMAND_RELOCATION_PREPARE) {
                        submittedPrepare.set(encoded.clone());
                    }
                    yield CompletableFuture.completedFuture(null);
                }
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
    }

    private static int attemptCount(
        ZLinkCanonicalRelocationStateMachine machine,
        String fieldName) {
        try {
            Field field = ZLinkCanonicalRelocationStateMachine.class
                .getDeclaredField(fieldName);
            field.setAccessible(true);
            return ((Map<?, ?>) field.get(machine)).size();
        } catch (ReflectiveOperationException failure) {
            throw new AssertionError(failure);
        }
    }

    private record ScheduledExpiry(
        Instant deadline,
        Runnable cleanup) {
    }

    private static final class CountingEndpoint
        implements ZLinkSpotRetireControl.TargetEndpoint {
        private final AtomicInteger staged = new AtomicInteger();
        private final AtomicInteger aborted = new AtomicInteger();
        private final AtomicInteger published = new AtomicInteger();
        private final boolean rejectStage;

        private CountingEndpoint() {
            this(false);
        }

        private CountingEndpoint(boolean rejectStage) {
            this.rejectStage = rejectStage;
        }

        @Override
        public CompletionStage<Void> stage(
            ZLinkSpotRetireControl.StageRequest request) {
            staged.incrementAndGet();
            return rejectStage
                ? CompletableFuture.failedFuture(
                    new IllegalStateException("target stage rejected"))
                : CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> publish(
            ZLinkSpotRetireControl.StageRequest request) {
            published.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> abort(
            ZLinkSpotRetireControl.StageRequest request) {
            aborted.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
    }
}
