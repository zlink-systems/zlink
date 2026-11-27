package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.testsupport.ZLinkTestRedis;

import java.time.Duration;
import java.util.List;
import java.util.UUID;

final class ZLinkAggregateCommitIdentityTest {
    private static final ZLinkStoreCancellation ACTIVE = () -> false;
    private static final String KEY = ZLinkAuthorityKeyCodec.spot("moved-room");
    private static final ZLinkPlacementCapacityBundle CAPACITY =
            ZLinkPlacementCapacityBundle.spot(ZLinkPlacementObjectKind.USER_SPOT, "room", 1);

    @ParameterizedTest(name = "Redis={0}")
    @ValueSource(booleans = {false, true})
    void commitSurvivesTargetDescriptorRemoval(boolean redis) throws Exception {
        try (var fixture = new Fixture(redis)) {
            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    fixture.repository
                            .removeMeshNode(fixture.target, fixture.targetOwner)
                            .toCompletableFuture()
                            .get());
            assertFalse(fixture.hasTargetDescriptor());
            assertInstanceOf(
                    ZLinkOwnerLeaseFound.class,
                    fixture.repository
                            .readOwnerLease(fixture.targetOwner.ownerId())
                            .toCompletableFuture()
                            .get());
            fixture.assertCommit();
        }
    }

    @ParameterizedTest(name = "Redis={0}")
    @ValueSource(booleans = {false, true})
    void commitSurvivesTargetLeaseExpiry(boolean redis) throws Exception {
        try (var fixture = new Fixture(redis)) {
            assertInstanceOf(
                    ZLinkOwnerLeaseRenewed.class,
                    fixture.repository
                            .renewOwnerLease(fixture.targetOwner, Duration.ofMillis(200))
                            .toCompletableFuture()
                            .get());
            long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
            var lease =
                    fixture.repository
                            .readOwnerLease(fixture.targetOwner.ownerId())
                            .toCompletableFuture()
                            .get();
            while (lease instanceof ZLinkOwnerLeaseFound found
                    && found.isLiveFor(
                            fixture.targetOwner.ownerId(), fixture.targetOwner.leaseGeneration())
                    && System.nanoTime() < deadline) {
                Thread.sleep(20);
                lease =
                        fixture.repository
                                .readOwnerLease(fixture.targetOwner.ownerId())
                                .toCompletableFuture()
                                .get();
            }
            assertTrue(
                    lease instanceof ZLinkOwnerLeaseMissing
                            || lease instanceof ZLinkOwnerLeaseFound found
                                    && !found.leaseExpiresAt().isAfter(found.storeNow()));
            assertTrue(fixture.hasTargetDescriptor());
            fixture.assertCommit();
        }
    }

    @ParameterizedTest(name = "Redis={0}")
    @ValueSource(booleans = {false, true})
    void commitRejectsMismatchedAggregateIdentity(boolean redis) throws Exception {
        try (var fixture = new Fixture(redis)) {
            var fence = fixture.fence;
            assertEquals(
                    ZLinkAggregateCommitResult.STALE,
                    fixture.commit(
                            new ZLinkAggregateFence(
                                    UUID.randomUUID(), fence.aggregateGeneration())));
            assertEquals(
                    ZLinkAggregateCommitResult.STALE,
                    fixture.commit(
                            new ZLinkAggregateFence(
                                    fence.aggregateId(), fence.aggregateGeneration() + 1)));
            var unchanged = fixture.snapshot();
            assertEquals(fixture.before.ownerId(), unchanged.ownerId());
            assertEquals(
                    fixture.before.authorityOwnerGeneration(),
                    unchanged.authorityOwnerGeneration());
            fixture.assertCommit();
        }
    }

    private static final class Fixture implements AutoCloseable {
        private final ZLinkRedisLocationStore redisStore;
        private final ZLinkLocationRepository repository;
        private final ZLinkLocationOwnerToken targetOwner;
        private final ZLinkMeshNodeDescriptorKey target;
        private final ZLinkAuthoritySnapshot before;
        private final ZLinkAggregateFence fence;

        Fixture(boolean redis) throws Exception {
            redisStore =
                    redis
                            ? new ZLinkRedisLocationStore(
                                    new ZLinkRedisLocationOptions()
                                            .setConnectionString(ZLinkTestRedis.endpoint())
                                            .setKeyPrefix(
                                                    "zlink:aggregate-commit-identity:"
                                                            + UUID.randomUUID()))
                            : null;
            repository =
                    redis
                            ? new ZLinkProviderLocationRepository(redisStore)
                            : new ZLinkInMemoryLocationStore();
            try {
                var sourceOwner = claim("source-owner");
                targetOwner = claim("target-owner");
                var source = publish("source", sourceOwner);
                var targetNode = publish("target", targetOwner);
                target = new ZLinkMeshNodeDescriptorKey(targetNode.meshName(), targetNode.rid());
                var request =
                        new ZLinkObjectReservationRequest(
                                ZLinkPlacementObjectKind.USER_SPOT,
                                KEY,
                                "room",
                                "inline-v1:test",
                                new byte[32],
                                4,
                                new ZLinkMeshNodeDescriptorKey(source.meshName(), source.rid()),
                                source.lifecycleGeneration(),
                                sourceOwner,
                                new byte[] {1},
                                CAPACITY);
                var reservation =
                        assertInstanceOf(
                                        ZLinkObjectReserved.class,
                                        repository
                                                .reserve(request, ACTIVE)
                                                .toCompletableFuture()
                                                .get())
                                .reservation();
                assertEquals(
                        ZLinkObjectCommitResult.COMMITTED,
                        repository
                                .commit(reservation, new byte[] {1}, ACTIVE)
                                .toCompletableFuture()
                                .get());
                before = snapshot();
                var participant =
                        new ZLinkAggregateParticipant(
                                KEY,
                                before.objectGeneration(),
                                before.authorityOwnerGeneration(),
                                before.storeVersion(),
                                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                new byte[] {2},
                                new byte[] {'[', ']'});
                var aggregate =
                        new ZLinkAggregatePrepareRequest(
                                UUID.randomUUID(),
                                1,
                                List.of(participant),
                                target,
                                targetNode.lifecycleGeneration(),
                                CAPACITY,
                                targetOwner);
                fence =
                        assertInstanceOf(
                                        ZLinkAggregatePrepared.class,
                                        repository
                                                .prepareAggregate(aggregate, ACTIVE)
                                                .toCompletableFuture()
                                                .get())
                                .fence();
            } catch (Exception | AssertionError failure) {
                close();
                throw failure;
            }
        }

        private ZLinkLocationOwnerToken claim(String ownerId) throws Exception {
            return assertInstanceOf(
                            ZLinkOwnerLeaseClaimed.class,
                            repository
                                    .claimOwnerLease(ownerId, Duration.ofMinutes(5))
                                    .toCompletableFuture()
                                    .get())
                    .token();
        }

        private ZLinkMeshNodeDescriptor publish(String nodeId, ZLinkLocationOwnerToken owner)
                throws Exception {
            var node =
                    ZLinkRedisAggregateRelocationCapacityLiveTest.descriptor(
                            RoutingId.from(nodeId), owner, 0);
            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    repository
                            .updateMeshNode(node, ZLinkLocationWriteIntent.NEW_CLAIM)
                            .toCompletableFuture()
                            .get()
                            .status());
            return node;
        }

        boolean hasTargetDescriptor() throws Exception {
            return repository
                    .listMeshNodes(target.meshName(), ZLinkPageRequest.firstPage())
                    .toCompletableFuture()
                    .get()
                    .items()
                    .stream()
                    .anyMatch(node -> node.rid().equals(target.rid()));
        }

        ZLinkAuthoritySnapshot snapshot() throws Exception {
            return assertInstanceOf(
                    ZLinkAuthoritySnapshot.class,
                    repository.read(KEY, ACTIVE).toCompletableFuture().get());
        }

        ZLinkAggregateCommitResult commit(ZLinkAggregateFence identity) throws Exception {
            return repository.commitAggregate(identity, ACTIVE).toCompletableFuture().get();
        }

        void assertCommit() throws Exception {
            assertEquals(ZLinkAggregateCommitResult.COMMITTED, commit(fence));
            var committed = snapshot();
            assertEquals(targetOwner.ownerId(), committed.ownerId());
            assertEquals(targetOwner.leaseGeneration(), committed.ownerLeaseGeneration());
            assertTrue(committed.authorityOwnerGeneration() > before.authorityOwnerGeneration());
        }

        @Override
        public void close() {
            if (redisStore != null) redisStore.close();
        }
    }
}
