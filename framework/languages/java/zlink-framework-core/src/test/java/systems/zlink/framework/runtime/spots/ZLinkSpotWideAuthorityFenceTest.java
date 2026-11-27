package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateParticipant;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepareRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepareResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepared;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityEntry;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectation;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityGenerationTransition;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMutation;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanCursor;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityStored;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityWriteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkCanonicalRelocationAuthorityStateCodecTestAccess;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkSpotWideAuthorityFenceTest {
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private static final Instant NOW = Instant.parse("2026-11-08T00:00:00Z");
    private static final RoutingId SOURCE = RoutingId.from("source-node");
    private static final RoutingId TARGET = RoutingId.from("target-node");
    private static final String SPOT = ZLinkAuthorityKeyCodec.spot("room");
    private static final String ACTOR = ZLinkAuthorityKeyCodec.actor("player");
    private static final String SOURCE_OWNER = "source-owner";
    private static final long LEASE = 3;
    private static final long NODE_GENERATION = 5;
    private static final long OBJECT_GENERATION = 7;
    private static final long OWNER_GENERATION = 11;
    private static final String SPOT_BASELINE = "spot-sealed";
    private static final String MEMBER_BASELINE = "member-sealed";

    @Test
    void sourceWritesPreparingAndCapturedOnTheRepresentativeAlone() {
        var store = new AuthorityFixture();
        var coordinator = new ZLinkAggregateRelocationCoordinator(store);
        var participants =
                List.of(
                        new ZLinkAggregateRelocationCoordinator.ExpectedParticipant(
                                ACTOR, OBJECT_GENERATION, OWNER_GENERATION),
                        new ZLinkAggregateRelocationCoordinator.ExpectedParticipant(
                                SPOT, OBJECT_GENERATION, OWNER_GENERATION));
        var fence = new ZLinkAggregateFence(UUID.randomUUID(), 1);
        coordinator
                .prepareSource(participants, SPOT, fence, 1, ZLinkAuthoritySnapshot::payload, OPEN)
                .toCompletableFuture()
                .join();
        coordinator.captureSource(participants, SPOT, fence, 1, OPEN).toCompletableFuture().join();
        assertEquals(List.of(SPOT + ":preparing", SPOT + ":captured"), store.writes);
        assertEquals(MEMBER_BASELINE, store.rows.get(ACTOR).storeVersion());
    }

    @Test
    void targetAcceptsCapturedRepresentativeWithSteadyMember() throws Exception {
        var store = new AuthorityFixture();
        var id = UUID.randomUUID();
        store.seedRepresentativeCapture(id);
        assertEquals(
                ServiceWireConstants.COMMAND_RELOCATION_READY,
                prepare(store, id, SOURCE_OWNER, LEASE, SPOT_BASELINE));
        assertEquals(
                MEMBER_BASELINE,
                store.prepared.participants().stream()
                        .filter(participant -> participant.authorityKey().equals(ACTOR))
                        .findFirst()
                        .orElseThrow()
                        .expectedStoreVersion());
    }

    @Test
    void targetRejectsVersionOnlyMemberWriteAfterSeal() throws Exception {
        var store = new AuthorityFixture();
        var id = UUID.randomUUID();
        store.seedRepresentativeCapture(id);
        store.touchMember();
        int reply = prepare(store, id, SOURCE_OWNER, LEASE, SPOT_BASELINE);
        assertEquals(SOURCE_OWNER, store.rows.get(ACTOR).ownerId());
        assertEquals(SOURCE_OWNER, store.rows.get(SPOT).ownerId());
        assertEquals(ServiceWireConstants.COMMAND_RELOCATION_FAILED, reply);
    }

    @Test
    void aggregatePrepareRejectsMemberVersionChangeAfterRestore() throws Exception {
        var store = new AuthorityFixture();
        var id = UUID.randomUUID();
        store.seedRepresentativeCapture(id);
        store.duringRestore = store::touchMember;

        int reply = prepare(store, id, SOURCE_OWNER, LEASE, SPOT_BASELINE);

        assertEquals(ServiceWireConstants.COMMAND_RELOCATION_FAILED, reply);
        assertNull(store.prepared);
        assertEquals(SOURCE_OWNER, store.rows.get(ACTOR).ownerId());
        assertEquals(SOURCE_OWNER, store.rows.get(SPOT).ownerId());
    }

    @ParameterizedTest
    @ValueSource(strings = {"owner", "lease", "baseline"})
    void targetRejectsChangedRepresentativeCoordinator(String change) throws Exception {
        var store = new AuthorityFixture();
        var id = UUID.randomUUID();
        store.seedRepresentativeCapture(id);
        int reply =
                prepare(
                        store,
                        id,
                        change.equals("owner") ? "other-owner" : SOURCE_OWNER,
                        change.equals("lease") ? LEASE + 1 : LEASE,
                        change.equals("baseline") ? "other-baseline" : SPOT_BASELINE);
        assertEquals(ServiceWireConstants.COMMAND_RELOCATION_FAILED, reply);
        assertNull(store.prepared);
        assertEquals(SOURCE_OWNER, store.rows.get(ACTOR).ownerId());
        assertEquals(SOURCE_OWNER, store.rows.get(SPOT).ownerId());
    }

    private static int prepare(
            AuthorityFixture store, UUID id, String owner, long lease, String baseline)
            throws Exception {
        var runtime =
                new ZLinkCanonicalRelocationStateMachine(
                        ZLinkCanonicalRelocationStateMachineTest.node(
                                TARGET, 17, new AtomicReference<>(), new ArrayList<>()),
                        "mesh",
                        "target-entry",
                        store,
                        new ZLinkAggregateRelocationCoordinator(store),
                        new ZLinkSpotRetireControl.TargetEndpoint() {
                            @Override
                            public CompletionStage<Void> stage(
                                    ZLinkSpotRetireControl.StageRequest request) {
                                store.duringRestore.run();
                                return CompletableFuture.completedFuture(null);
                            }

                            @Override
                            public CompletionStage<Void> publish(
                                    ZLinkSpotRetireControl.StageRequest request) {
                                throw new AssertionError("PREPARE alone cannot publish the target");
                            }

                            @Override
                            public CompletionStage<Void> abort(
                                    ZLinkSpotRetireControl.StageRequest request) {
                                return CompletableFuture.completedFuture(null);
                            }
                        });
        byte[] root =
                ServiceWirePilotCodec.encodeRelocationEnvelopeV1(
                        new ServiceWirePilotCodec.RelocationEnvelopeV1(
                                id.getMostSignificantBits(),
                                id.getLeastSignificantBits(),
                                new ServiceWirePilotCodec.RelocationUserSpotIdentity(
                                        "room", OBJECT_GENERATION, OWNER_GENERATION),
                                1,
                                List.of(
                                        new ServiceWirePilotCodec.RelocationApplicationState(
                                                1, false, new byte[0]),
                                        new ServiceWirePilotCodec.RelocationApplicationState(
                                                2, false, new byte[0])),
                                List.of(),
                                List.of(),
                                List.of(),
                                List.of()));
        var coordinator =
                new ZLinkCanonicalRelocationProtocol.Coordinator(
                        owner, lease, SOURCE, NODE_GENERATION, baseline);
        var object =
                new ZLinkCanonicalRelocationProtocol.ObjectFence(
                        ZLinkPlacementObjectKind.USER_SPOT.value(),
                        "room",
                        "",
                        OBJECT_GENERATION,
                        OWNER_GENERATION);
        var prepare =
                new ZLinkCanonicalRelocationProtocol.Prepare(
                        id,
                        1,
                        coordinator,
                        new ZLinkCanonicalRelocationProtocol.Target(TARGET, 17, "target-owner", 13),
                        ZLinkCanonicalRelocationProtocol.SOURCE,
                        object,
                        SOURCE,
                        NODE_GENERATION,
                        ZLinkRelocationPayloadTransfer.manifest(root, root.length),
                        1,
                        Instant.parse("2100-01-01T00:00:00Z").toEpochMilli());
        var reply =
                runtime.apply(
                                SOURCE,
                                1L,
                                ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
                                ZLinkCanonicalRelocationProtocol.encodePrepare(prepare))
                        .toCompletableFuture();
        runtime.apply(
                        SOURCE,
                        ServiceWireConstants.COMMAND_RELOCATION_STATE,
                        ZLinkCanonicalRelocationProtocol.encodeState(
                                new ZLinkCanonicalRelocationProtocol.State(
                                        id,
                                        1,
                                        coordinator,
                                        ZLinkCanonicalRelocationProtocol.SOURCE,
                                        object,
                                        0,
                                        root)))
                .toCompletableFuture()
                .join();
        return Byte.toUnsignedInt(reply.get(5, TimeUnit.SECONDS)[3]);
    }

    private static final class AuthorityFixture extends ZLinkLocationStoreTestAdapter
            implements ZLinkLocationStore {
        private final ZLinkInMemoryProviderLocationStore inventory =
                new ZLinkInMemoryProviderLocationStore();
        private final List<String> writes = new ArrayList<>();
        private final Map<String, ZLinkAuthoritySnapshot> rows = new HashMap<>();
        private ZLinkAggregatePrepareRequest prepared;
        private Runnable duringRestore = () -> {};

        AuthorityFixture() {
            rows.put(
                    SPOT,
                    steady(
                            SPOT_BASELINE,
                            ZLinkPlacementObjectKind.USER_SPOT,
                            "room-type",
                            ZLinkPlacementCapacityBundle.spot(
                                    ZLinkPlacementObjectKind.USER_SPOT, "room-type", 1),
                            new ZLinkServiceAuthorityPayloadCodec()
                                    .encodeUser(
                                            ZLinkServiceAuthorityPayloadCodec.State.READY,
                                            "room-type",
                                            "room",
                                            SOURCE_OWNER,
                                            LEASE,
                                            "mesh",
                                            SOURCE,
                                            NODE_GENERATION)));
            rows.put(
                    ACTOR,
                    steady(
                            MEMBER_BASELINE,
                            ZLinkPlacementObjectKind.ACTOR,
                            "player-type",
                            ZLinkPlacementCapacityBundle.actor(1),
                            new ZLinkActorAuthorityPayloadCodec()
                                    .encode(
                                            ZLinkActorAuthorityPayloadCodec.State.READY,
                                            "player-type",
                                            "player",
                                            "room",
                                            OBJECT_GENERATION,
                                            2,
                                            SOURCE_OWNER,
                                            LEASE,
                                            "mesh",
                                            SOURCE,
                                            NODE_GENERATION)));
        }

        private static ZLinkAuthoritySnapshot steady(
                String version,
                ZLinkPlacementObjectKind kind,
                String type,
                ZLinkPlacementCapacityBundle capacity,
                byte[] payload) {
            return new ZLinkAuthoritySnapshot(
                    version,
                    payload,
                    OBJECT_GENERATION,
                    OWNER_GENERATION,
                    SOURCE_OWNER,
                    LEASE,
                    new ZLinkPlacementAllocation(
                            ZLinkPlacementAllocationState.ACTIVE,
                            kind,
                            type,
                            new ZLinkMeshNodeDescriptorKey("mesh", SOURCE),
                            NODE_GENERATION,
                            capacity),
                    NOW);
        }

        void touchMember() {
            var before = rows.get(ACTOR);
            compareExchange(
                            ACTOR,
                            new ZLinkAuthorityExpectFound(before.storeVersion()),
                            new ZLinkAuthorityPut(before.payload()),
                            OPEN)
                    .toCompletableFuture()
                    .join();
        }

        void seedRepresentativeCapture(UUID id) {
            var participants =
                    List.of(ACTOR, SPOT).stream()
                            .map(
                                    key -> {
                                        var snapshot = rows.get(key);
                                        return new ZLinkAggregateParticipant(
                                                key,
                                                snapshot.objectGeneration(),
                                                snapshot.authorityOwnerGeneration(),
                                                snapshot.storeVersion(),
                                                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                                snapshot.payload(),
                                                "[]".getBytes(StandardCharsets.UTF_8));
                                    })
                            .toList();
            var digest =
                    sealAggregateInventory(new ZLinkAggregateFence(id, 1), participants, OPEN)
                            .toCompletableFuture()
                            .join();
            var before = rows.get(SPOT);
            var put =
                    ZLinkCanonicalRelocationAuthorityStateCodecTestAccess.sourcePhaseMutation(
                            before,
                            id,
                            "captured",
                            1,
                            before.storeVersion(),
                            false,
                            HexFormat.of().formatHex(digest));
            compareExchange(SPOT, new ZLinkAuthorityExpectFound(before.storeVersion()), put, OPEN)
                    .toCompletableFuture()
                    .join();
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return inventory.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return inventory.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return inventory.scan(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkAuthorityReadResult> read(
                String key, ZLinkStoreCancellation cancellation) {
            return CompletableFuture.completedFuture(rows.get(key));
        }

        @Override
        public CompletionStage<ZLinkAuthorityScanResult> list(
                String prefix,
                Optional<ZLinkAuthorityScanCursor> cursor,
                int limit,
                ZLinkStoreCancellation cancellation) {
            return CompletableFuture.completedFuture(
                    new ZLinkAuthorityPage(
                            List.of(new ZLinkAuthorityEntry(ACTOR, rows.get(ACTOR))),
                            Optional.empty()));
        }

        @Override
        public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
                String key,
                ZLinkAuthorityExpectation expectation,
                ZLinkAuthorityMutation mutation,
                ZLinkStoreCancellation cancellation) {
            var before = rows.get(key);
            assertEquals(new ZLinkAuthorityExpectFound(before.storeVersion()), expectation);
            var put = (ZLinkAuthorityPut) mutation;
            var relocation = put.replaceRelocation() ? put.relocation() : before.relocation();
            var version = "version-" + writes.size();
            writes.add(key + ":" + (relocation == null ? "steady" : relocation.phase()));
            var after =
                    new ZLinkAuthoritySnapshot(
                            version,
                            put.payload(),
                            before.objectGeneration(),
                            before.authorityOwnerGeneration(),
                            before.ownerId(),
                            before.ownerLeaseGeneration(),
                            before.allocation(),
                            Optional.empty(),
                            NOW,
                            relocation,
                            0);
            rows.put(key, after);
            return CompletableFuture.completedFuture(
                    new ZLinkAuthorityStored(
                            version,
                            after.payload(),
                            after.objectGeneration(),
                            after.authorityOwnerGeneration(),
                            after.ownerId(),
                            after.ownerLeaseGeneration(),
                            after.allocation(),
                            NOW));
        }

        @Override
        public CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
                ZLinkAggregatePrepareRequest request, ZLinkStoreCancellation cancellation) {
            for (var participant : request.participants()) {
                if (!rows.get(participant.authorityKey())
                        .storeVersion()
                        .equals(participant.casStoreVersion()))
                    return CompletableFuture.completedFuture(new ZLinkAggregateConflict());
            }
            prepared = request;
            return CompletableFuture.completedFuture(
                    new ZLinkAggregatePrepared(
                            new ZLinkAggregateFence(
                                    request.aggregateId(), request.aggregateGeneration())));
        }
    }
}
