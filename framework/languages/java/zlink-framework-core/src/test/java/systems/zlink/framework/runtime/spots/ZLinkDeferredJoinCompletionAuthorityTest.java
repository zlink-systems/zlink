package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationStore;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationDeleteResult;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationFound;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationMissing;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationReadResult;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationRenewMissing;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationRenewResult;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationRenewed;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationStored;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.CRC32C;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkDeferredJoinCompletionAuthority;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

final class ZLinkDeferredJoinCompletionAuthorityTest {
    private static final ZLinkStoreCancellation NEVER = () -> false;

    @Test
    void canonicalAuthorityKeepsEachCursorAndClearsReferenceAfterSourceCleanup() {
        MemoryLocationStore locations = new MemoryLocationStore();
        MemoryRelocationStore roots = new MemoryRelocationStore();
        String actorId = "actor-a";
        String authorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        UUID relocationId = new UUID(17, 29);
        byte[] steady = new ZLinkActorAuthorityPayloadCodec().encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "Player",
            actorId,
            "entry-a",
            1,
            1,
            "owner-a",
            5,
            "game",
            RoutingId.from("node-a"),
            7);
        byte[] root = ZLinkCanonicalActorRelocationEnvelope.encode(
            relocationId,
            actorId,
            17,
            9,
            false,
            new byte[0],
            List.of());
        var source = new ZLinkAggregateRelocationCoordinator.Participant(
            authorityKey,
            ZLinkPlacementObjectKind.ACTOR,
            17,
            9,
            "source-v1",
            ZLinkAuthorityGenerationTransition.NEW_OWNER,
            steady,
            new byte[] {1});
        var request = new ZLinkAggregateRelocationCoordinator.Request(
            relocationId,
            1,
            List.of(source),
            root,
            new ZLinkMeshNodeDescriptorKey(
                "game", RoutingId.from("node-b")),
            11,
            ZLinkPlacementCapacityBundle.actor(1),
            new ZLinkLocationOwnerToken("owner-b", 12));
        locations.source = source;
        var aggregate = new ZLinkAggregateRelocationCoordinator(
            locations, roots);
        aggregate.commit(
                aggregate.prepare(request, NEVER).toCompletableFuture().join(),
                NEVER)
            .toCompletableFuture().join();

        var journal = new ZLinkDeferredJoinCompletionAuthority(
            locations, roots);
        var operation = new ZLinkActorJoinOperationId(41, 73);
        var actor = new ZLinkBackendActorRef(
            RoutingId.from("node-b"), actorId, 17);
        byte[] reply = "\"accepted\"".getBytes(
            java.nio.charset.StandardCharsets.UTF_8);

        journal.awaitTargetCommit(
                relocationId,
                1,
                actor,
                Duration.ofSeconds(1))
            .toCompletableFuture().join();

        var prepared = journal.prepare(operation, actor, reply)
            .toCompletableFuture().join();
        assertEquals(1, prepared.cursor());
        assertPublished(
            journal, locations, roots, authorityKey, operation, actor, 1);

        var committed = journal.advance(prepared, actor, 2)
            .toCompletableFuture().join();
        assertEquals(2, committed.cursor());
        assertPublished(
            journal, locations, roots, authorityKey, operation, actor, 2);

        assertThrows(
            java.util.concurrent.CompletionException.class,
            () -> journal.awaitTargetCompletion(
                    relocationId,
                    1,
                    operation,
                    actor,
                    Duration.ofMillis(20))
                .toCompletableFuture().join());

        journal.markSourceCleanup(operation, actor)
            .toCompletableFuture().join();
        journal.awaitSourceCleanup(
                relocationId,
                1,
                actor,
                Duration.ofSeconds(1))
            .toCompletableFuture().join();

        var delivered = journal.advance(committed, actor, 3)
            .toCompletableFuture().join();
        assertEquals(3, delivered.cursor());
        assertPublished(
            journal, locations, roots, authorityKey, operation, actor, 3);
        journal.awaitTargetCompletion(
                relocationId,
                1,
                operation,
                actor,
                Duration.ofSeconds(1))
            .toCompletableFuture().join();

        journal.completeSourceCleanupAndRelease(delivered, actor)
            .toCompletableFuture().join();

        var released = new ZLinkActorAuthorityPayloadCodec()
            .decode(locations.rows.get(authorityKey).payload())
            .orElseThrow();
        assertEquals("owner-b", released.ownerId());
        assertEquals(12, released.ownerLeaseGeneration());
        assertEquals(RoutingId.from("node-b"), released.nodeRid());
        assertEquals(11, released.nodeGeneration());
        assertEquals(17, locations.rows.get(authorityKey).objectGeneration());
        assertEquals(10,
            locations.rows.get(authorityKey).authorityOwnerGeneration());
        assertFalse(roots.values.containsKey(delivered.reference()),
            "authority clear precedes deletion of the Delivered manifest");
    }

    private static void assertPublished(
        ZLinkDeferredJoinCompletionAuthority journal,
        MemoryLocationStore locations,
        MemoryRelocationStore roots,
        String authorityKey,
        ZLinkActorJoinOperationId operation,
        ZLinkBackendActorRef actor,
        int cursor) {
        ZLinkAuthoritySnapshot snapshot = locations.rows.get(authorityKey);
        String reference = locations.reference(snapshot.payload());
        assertTrue(
            roots.values.containsKey(reference),
            "Location authority must reference a stored manifest");
        assertEquals(
            cursor,
            journal.recover(operation, actor)
                .toCompletableFuture().join().cursor());
    }

    private static final class MemoryLocationStore
        extends ZLinkLocationStoreTestAdapter {
        private final Map<String, ZLinkAuthoritySnapshot> rows =
            new ConcurrentHashMap<>();
        private final AtomicInteger versions = new AtomicInteger();
        private ZLinkAggregatePrepareRequest prepared;
        private ZLinkAggregateRelocationCoordinator.Participant source;
        private ZLinkAggregateProgress progress;
        private String progressStoreVersion;

        @Override
        public CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
            ZLinkAggregatePrepareRequest request,
            ZLinkStoreCancellation cancellation) {
            prepared = request;
            return CompletableFuture.completedFuture(
                new ZLinkAggregatePrepared(new ZLinkAggregateFence(
                    request.aggregateId(),
                    request.aggregateGeneration())));
        }

        @Override
        public CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation) {
            ZLinkAggregateParticipant participant =
                prepared.participants().getFirst();
            rows.put(participant.authorityKey(), snapshot(
                participant.authorityPayload(),
                source.authorityOwnerGeneration() + 1,
                prepared.targetOwner().ownerId(),
                prepared.targetOwner().leaseGeneration()));
            progress = ZLinkCanonicalRelocationAuthorityStateCodec.progress(
                participant.authorityPayload());
            progressStoreVersion = "aggregate-commit";
            return CompletableFuture.completedFuture(
                ZLinkAggregateCommitResult.COMMITTED);
        }

        @Override
        public CompletionStage<Optional<ZLinkAggregateProgressSnapshot>>
            readAggregateProgress(
                ZLinkAggregateFence fence,
                ZLinkStoreCancellation cancellation) {
            return CompletableFuture.completedFuture(
                progress == null
                    ? Optional.empty()
                    : Optional.of(progressSnapshot(fence)));
        }

        @Override
        public CompletionStage<ZLinkAggregateProgressWriteResult>
            compareExchangeAggregateProgress(
                ZLinkAggregateFence fence,
                String expectedStoreVersion,
                ZLinkAggregateProgress next,
                ZLinkStoreCancellation cancellation) {
            if (progress == null
                || !progressStoreVersion.equals(expectedStoreVersion)) {
                return CompletableFuture.completedFuture(
                    new ZLinkAggregateProgressConflict());
            }
            progress = next;
            progressStoreVersion = "aggregate-progress";
            return CompletableFuture.completedFuture(
                new ZLinkAggregateProgressStored(progressSnapshot(fence)));
        }

        @Override
        public CompletionStage<List<ZLinkAggregateProgressSnapshot>>
            listAggregateProgress(ZLinkStoreCancellation cancellation) {
            return CompletableFuture.completedFuture(
                progress == null
                    ? List.of()
                    : List.of(progressSnapshot(new ZLinkAggregateFence(
                        prepared.aggregateId(),
                        prepared.aggregateGeneration()))));
        }

        @Override
        public CompletionStage<Boolean> removeAggregateProgress(
            ZLinkAggregateFence fence,
            String expectedStoreVersion,
            ZLinkStoreCancellation cancellation) {
            if (progress == null
                || !progressStoreVersion.equals(expectedStoreVersion)) {
                return CompletableFuture.completedFuture(false);
            }
            progress = null;
            progressStoreVersion = null;
            return CompletableFuture.completedFuture(true);
        }

        private ZLinkAggregateProgressSnapshot progressSnapshot(
            ZLinkAggregateFence fence) {
            return new ZLinkAggregateProgressSnapshot(
                fence,
                progressStoreVersion,
                prepared,
                progress);
        }

        @Override
        public CompletionStage<ZLinkAuthorityReadResult> read(
            String key,
            ZLinkStoreCancellation cancellation) {
            ZLinkAuthoritySnapshot value = rows.get(key);
            return CompletableFuture.completedFuture(value == null
                ? new ZLinkAuthorityMissing(Instant.now())
                : value);
        }

        @Override
        public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
            String key,
            ZLinkAuthorityExpectation expectation,
            ZLinkAuthorityMutation mutation,
            ZLinkStoreCancellation cancellation) {
            ZLinkAuthoritySnapshot current = rows.get(key);
            if (!(expectation instanceof ZLinkAuthorityExpectFound found)
                || !(mutation instanceof ZLinkAuthorityPut put)
                || current == null
                || !current.storeVersion().equals(found.storeVersion())) {
                return CompletableFuture.completedFuture(
                    new ZLinkAuthorityConflict(current == null
                        ? new ZLinkAuthorityMissing(Instant.now())
                        : current));
            }
            ZLinkAuthoritySnapshot stored = snapshot(
                put.payload(),
                current.authorityOwnerGeneration(),
                current.ownerId(),
                current.ownerLeaseGeneration());
            rows.put(key, stored);
            return CompletableFuture.completedFuture(new ZLinkAuthorityStored(
                stored.storeVersion(),
                stored.payload(),
                stored.objectGeneration(),
                stored.authorityOwnerGeneration(),
                stored.ownerId(),
                stored.ownerLeaseGeneration(),
                stored.allocation(),
                stored.storeNow()));
        }

        private ZLinkAuthoritySnapshot snapshot(
            byte[] payload,
            long ownerGeneration,
            String ownerId,
            long ownerLeaseGeneration) {
            return new ZLinkAuthoritySnapshot(
                "v-" + versions.incrementAndGet(),
                payload,
                17,
                ownerGeneration,
                ownerId,
                ownerLeaseGeneration,
                new ZLinkPlacementAllocation(
                    ZLinkPlacementAllocationState.ACTIVE,
                    ZLinkPlacementObjectKind.ACTOR,
                    "Player",
                    new ZLinkMeshNodeDescriptorKey(
                        "game", RoutingId.from("node-b")),
                    11,
                    ZLinkPlacementCapacityBundle.actor(1)),
                Instant.now());
        }

        private String reference(byte[] payload) {
            // The canonical pointer is the only text16 value starting with
            // this deterministic in-memory prefix.
            String text = new String(
                payload, java.nio.charset.StandardCharsets.ISO_8859_1);
            int offset = text.indexOf("root-");
            assertTrue(offset >= 2);
            int length = Byte.toUnsignedInt(payload[offset - 1]);
            return new String(
                payload,
                offset,
                length,
                java.nio.charset.StandardCharsets.UTF_8);
        }
    }

    private static final class MemoryRelocationStore
        implements ZLinkRelocationStore {
        private final Map<String, byte[]> values = new ConcurrentHashMap<>();
        private final AtomicInteger sequence = new AtomicInteger();

        @Override
        public CompletionStage<ZLinkRelocationStored> put(
            byte[] payload,
            Duration retention,
            ZLinkStoreCancellation cancellation) {
            String reference = "root-" + sequence.incrementAndGet();
            values.put(reference, payload.clone());
            Instant now = Instant.now();
            CRC32C checksum = new CRC32C();
            checksum.update(payload, 0, payload.length);
            return CompletableFuture.completedFuture(new ZLinkRelocationStored(
                reference,
                checksum.getValue(),
                now.plus(retention),
                now));
        }

        @Override
        public CompletionStage<ZLinkRelocationReadResult> get(
            String reference,
            ZLinkStoreCancellation cancellation) {
            byte[] payload = values.get(reference);
            return CompletableFuture.completedFuture(payload == null
                ? new ZLinkRelocationMissing()
                : new ZLinkRelocationFound(payload));
        }

        @Override
        public CompletionStage<ZLinkRelocationRenewResult> renew(
            String reference,
            Duration retention,
            ZLinkStoreCancellation cancellation) {
            return CompletableFuture.completedFuture(
                new ZLinkRelocationRenewed(
                    Instant.now().plus(retention),
                    Instant.now()));
        }

        @Override
        public CompletionStage<ZLinkRelocationDeleteResult> delete(
            String reference,
            ZLinkStoreCancellation cancellation) {
            return CompletableFuture.completedFuture(
                values.remove(reference) == null
                    ? ZLinkRelocationDeleteResult.MISSING
                    : ZLinkRelocationDeleteResult.DELETED);
        }
    }
}
