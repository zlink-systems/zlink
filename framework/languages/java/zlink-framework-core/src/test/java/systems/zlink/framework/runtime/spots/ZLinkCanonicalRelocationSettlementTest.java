package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
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
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Relocation settlement after relay readiness (spec 28 §4.4–§4.5, spec 01 §6.1/§10): the cutover
 * wait is a Warning only, a resent whole batch replaces a partial one, the source settles from the
 * Location Store, and a won source {@code Preserve} fence ends the target staging.
 */
final class ZLinkCanonicalRelocationSettlementTest {
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private static final byte[] R1 = {11, 1};
    private static final byte[] R2 = {22, 2, 2};

    @Test
    void cutoverWaitIsAWarningOnlyAndNeverStartsTheTargetCas() throws Exception {
        Fixture fixture = new Fixture();
        fixture.dropCutover.set(true);
        fixture.stageAndRelay(R1);

        assertThrows(CompletionException.class, fixture::publish);
        fixture.cutoverWarning.get().run();

        assertEquals(0, fixture.commits.get(), "no target CAS without a verified CUTOVER");
        assertEquals(0, fixture.endpoint.published.get(), "dispatch stays closed");
        assertEquals(List.of(), fixture.endpoint.relayed(), "relay is staged only after CUTOVER");
    }

    @Test
    void resentWholeBatchReplacesThePartialSectionBeforeTheVerifiedCutover() throws Exception {
        Fixture fixture = new Fixture();
        fixture.dropCutover.set(true);
        fixture.dropData.set(record -> record[0] == R2[0]);
        fixture.stageAndRelay(R1, R2);
        assertThrows(CompletionException.class, fixture::publish);

        //  The new connection carries the whole pre-boundary batch, then the cutover.
        fixture.applyToTarget(ServiceWireConstants.COMMAND_RELOCATION_DATA, fixture.data.get(0));
        fixture.applyToTarget(ServiceWireConstants.COMMAND_RELOCATION_DATA, fixture.data.get(1));
        fixture.applyToTarget(
                ServiceWireConstants.COMMAND_RELOCATION_CUTOVER, fixture.cutover.get());

        fixture.endpoint.publishedEvent.get(3, TimeUnit.SECONDS);
        assertEquals(1, fixture.commits.get());
        List<byte[]> relayed = fixture.endpoint.relayed();
        assertEquals(2, relayed.size(), "each pre-boundary record is staged once, in order");
        assertArrayEquals(R1, relayed.get(0));
        assertArrayEquals(R2, relayed.get(1));
    }

    @Test
    void sourcePreserveFenceWinsAtTheRestoreDeadlineAndTheTargetDiscardsStaging() throws Exception {
        Fixture fixture = new Fixture();
        fixture.dropCutover.set(true);
        fixture.stageAndRelay(R1);
        assertThrows(CompletionException.class, fixture::publish);
        String versionBefore = fixture.authority().storeVersion();

        ZLinkRelocationTransitionClient.Settlement settlement =
                fixture.source
                        .settle(fixture.targetRid, fixture.request.fence(), Instant.now())
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS);

        assertEquals(ZLinkRelocationTransitionClient.Settlement.SOURCE_PRESERVED, settlement);
        ZLinkAuthoritySnapshot after = fixture.authority();
        assertEquals(fixture.sourceOwner.ownerId(), after.ownerId(), "the source stays owner");
        assertNotEquals(versionBefore, after.storeVersion(), "Preserve changes the StoreVersion");
        fixture.cutoverWarning.get().run();
        fixture.endpoint.abortedEvent.get(3, TimeUnit.SECONDS);

        fixture.applyToTarget(
                ServiceWireConstants.COMMAND_RELOCATION_CUTOVER, fixture.cutover.get());
        assertEquals(0, fixture.commits.get(), "a late CUTOVER does not change settled authority");
        assertEquals(0, fixture.endpoint.published.get());
    }

    @Test
    void sourceResendsTheWholeBatchAndCutoverAfterReconnectAndConfirmsTheTargetCommit()
            throws Exception {
        Fixture fixture = new Fixture();
        fixture.stage();
        fixture.connected.set(false);
        fixture.dropData.set(record -> true);
        fixture.dropCutover.set(true);
        fixture.relay(R1, R2);
        assertThrows(CompletionException.class, fixture::publish);

        CompletableFuture<ZLinkRelocationTransitionClient.Settlement> settlement =
                fixture.source
                        .settle(
                                fixture.targetRid,
                                fixture.request.fence(),
                                Instant.now().plusSeconds(30))
                        .toCompletableFuture();
        assertEquals(0, fixture.commits.get());

        fixture.dropData.set(record -> false);
        fixture.dropCutover.set(false);
        fixture.connected.set(true);

        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED,
                settlement.get(3, TimeUnit.SECONDS));
        assertEquals(1, fixture.commits.get());
        List<byte[]> relayed = fixture.endpoint.relayed();
        assertEquals(2, relayed.size());
        assertArrayEquals(R1, relayed.get(0));
        assertArrayEquals(R2, relayed.get(1));
        assertEquals(fixture.targetOwner.ownerId(), fixture.authority().ownerId());
    }

    @Test
    void expiredSourceLeaseEndsSettlementBeforeTheRestoreDeadline() throws Exception {
        Fixture fixture = new Fixture();
        fixture.dropCutover.set(true);
        fixture.stageAndRelay(R1);
        assertThrows(CompletionException.class, fixture::publish);
        fixture.locations.releaseOwnerLease(fixture.sourceOwner).toCompletableFuture().join();

        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.SOURCE_LEASE_EXPIRED,
                fixture.source
                        .settle(
                                fixture.targetRid,
                                fixture.request.fence(),
                                Instant.now().plusSeconds(30))
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS));
        assertEquals(0, fixture.commits.get());
    }

    private static final class Fixture {
        final RoutingId sourceRid = RoutingId.from("source-node");
        final RoutingId targetRid = RoutingId.from("target-node");
        final ZLinkInMemoryLocationStore locations = new ZLinkInMemoryLocationStore();
        final AtomicInteger commits = new AtomicInteger();
        final AtomicBoolean connected = new AtomicBoolean(true);
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

        Fixture() {
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
                                        if (method.getName().equals("commitAggregate")) {
                                            commits.incrementAndGet();
                                        }
                                        try {
                                            return method.invoke(locations, args);
                                        } catch (InvocationTargetException failure) {
                                            throw failure.getCause();
                                        }
                                    });
            var coordinator = new ZLinkAggregateRelocationCoordinator(observed);
            UUID relocationId = UUID.randomUUID();
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
                            options);
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
                                } else {
                                    CompletableFuture.delayedExecutor(
                                                    delay.toMillis(), TimeUnit.MILLISECONDS)
                                            .execute(task);
                                }
                            });
            sourceRef.set(source);
            targetRef.set(target);
        }

        void stage() {
            source.stage(targetRid, request, Duration.ofSeconds(2)).toCompletableFuture().join();
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
                                        case "sendCanonicalRelocationControl" -> {
                                            byte[] encoded = ((byte[]) args[1]).clone();
                                            int command = Byte.toUnsignedInt(encoded[3]);
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

    private static final class RecordingEndpoint implements ZLinkSpotRetireControl.TargetEndpoint {
        final AtomicInteger aborted = new AtomicInteger();
        final AtomicInteger published = new AtomicInteger();
        final CompletableFuture<Void> abortedEvent = new CompletableFuture<>();
        final CompletableFuture<Void> publishedEvent = new CompletableFuture<>();
        private final List<byte[]> relayed = new CopyOnWriteArrayList<>();

        List<byte[]> relayed() {
            return new ArrayList<>(relayed);
        }

        @Override
        public CompletionStage<Void> stage(ZLinkSpotRetireControl.StageRequest request) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> stageRelayedRecord(
                ZLinkSpotRetireControl.StageRequest request, byte[] frozenRecord) {
            relayed.add(frozenRecord.clone());
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> publish(ZLinkSpotRetireControl.StageRequest request) {
            published.incrementAndGet();
            publishedEvent.complete(null);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> abort(ZLinkSpotRetireControl.StageRequest request) {
            aborted.incrementAndGet();
            abortedEvent.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }
}
