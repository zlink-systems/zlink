package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.time.Instant;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.zip.CRC32C;
import java.util.regex.Pattern;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

final class ZLinkAggregateRelocationCoordinatorTest {
    private static final ZLinkStoreCancellation NEVER = () -> false;

    @Test
    void targetCanStageAfterPrepareAndBeforeAtomicPublication() {
        FakeAuthorityStore authority = new FakeAuthorityStore();
        FakeRelocationStore relocation = new FakeRelocationStore();
        ZLinkAggregateRelocationCoordinator coordinator =
            new ZLinkAggregateRelocationCoordinator(authority, relocation);

        var prepared = coordinator.prepare(request(), NEVER)
            .toCompletableFuture().join();
        assertEquals(1, authority.prepareCount);
        assertEquals(0, authority.commitCount,
            "target factory and Restore staging precede publication");
        assertArrayEquals(
            authority.prepared.inventoryDigest(),
            prepared.inventoryDigest());
        assertEquals(2, authority.prepared.participants().size());

        var published = coordinator.commit(prepared, NEVER)
            .toCompletableFuture().join();
        assertEquals(1, authority.commitCount);
        assertEquals(prepared.fence(), published.fence());
        assertEquals(2, relocation.renewCount,
            "one chunk and the manifest are renewed");
    }

    @Test
    void prepareConflictDeletesOnlyTheUnpublishedRoot() {
        FakeAuthorityStore authority = new FakeAuthorityStore();
        authority.prepareResult = new ZLinkAggregateConflict();
        FakeRelocationStore relocation = new FakeRelocationStore();
        ZLinkAggregateRelocationCoordinator coordinator =
            new ZLinkAggregateRelocationCoordinator(authority, relocation);

        var failure = assertThrows(
            java.util.concurrent.CompletionException.class,
            () -> coordinator.prepare(request(), NEVER)
                .toCompletableFuture().join());
        assertInstanceOf(
            ZLinkAggregateRelocationCoordinator.AuthorityConflictException.class,
            failure.getCause());
        assertEquals(1, relocation.deleteCount,
            "prepare conflict removes only the unpublished manifest");
        assertEquals(0, authority.commitCount);
    }

    @Test
    void explicitAbortReleasesPreparedAggregateBeforeDeletingRoot() {
        FakeAuthorityStore authority = new FakeAuthorityStore();
        FakeRelocationStore relocation = new FakeRelocationStore();
        ZLinkAggregateRelocationCoordinator coordinator =
            new ZLinkAggregateRelocationCoordinator(authority, relocation);
        var prepared = coordinator.prepare(request(), NEVER)
            .toCompletableFuture().join();

        coordinator.abort(prepared).toCompletableFuture().join();

        assertEquals(1, authority.abortCount);
        assertEquals(1, relocation.deleteCount,
            "abort removes only the unpublished manifest");
    }

    @Test
    void sourceCleanupPublishesWithPreservedAggregateGeneration() {
        FakeAuthorityStore authority = new FakeAuthorityStore();
        FakeRelocationStore relocation = new FakeRelocationStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var activated = coordinator.commit(
                coordinator.prepare(request(), NEVER)
                    .toCompletableFuture().join(),
                NEVER)
            .toCompletableFuture().join();

        var completed = coordinator.completeSourceCleanup(
                activated,
                goldenRoot(),
                NEVER)
            .toCompletableFuture().join();

        assertEquals(7, completed.fence().aggregateGeneration());
        assertEquals(1, authority.commitCount,
            "source cleanup uses participant StoreVersion CAS");
        assertTrue(authority.progress.sourceCleanupCompleted(),
            "cleanup phase is owned by the aggregate marker");
        assertEquals(0, relocation.deleteCount,
            "cleanup keeps a root that remains referenced after CAS");
    }

    @Test
    void completedAggregateNormalizesToSteadyAuthorityWithoutGenerationChange() {
        FakeAuthorityStore authority = new FakeAuthorityStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            new FakeRelocationStore());
        var source = request();
        var activated = coordinator.commit(
                coordinator.prepare(source, NEVER)
                    .toCompletableFuture().join(),
                NEVER)
            .toCompletableFuture().join();
        coordinator.completeSourceCleanup(activated, goldenRoot(), NEVER)
            .toCompletableFuture().join();
        var expected = source.participants().stream()
            .map(value -> new ZLinkAggregateRelocationCoordinator
                .ExpectedParticipant(
                    value.authorityKey(),
                    value.objectGeneration(),
                    value.authorityOwnerGeneration()))
            .toList();

        coordinator.normalizeCompletedAggregate(
                expected,
                activated.fence(),
                source.targetOwner(),
                NEVER)
            .toCompletableFuture().join();

        for (var participant : source.participants()) {
            ZLinkAuthoritySnapshot normalized = authority.rows.get(
                participant.authorityKey());
            if (participant.objectKind() == ZLinkPlacementObjectKind.ACTOR) {
                var actor = new ZLinkActorAuthorityPayloadCodec()
                    .decode(normalized.payload())
                    .orElseThrow();
                assertEquals("owner-b", actor.ownerId());
                assertEquals(12, actor.ownerLeaseGeneration());
            } else {
                var spot = new ZLinkServiceAuthorityPayloadCodec()
                    .decode(normalized.payload())
                    .orElseThrow();
                assertEquals("owner-b", spot.ownerId());
                assertEquals(12, spot.ownerLeaseGeneration());
            }
            assertEquals(
                participant.authorityOwnerGeneration() + 1,
                normalized.authorityOwnerGeneration());
            assertEquals(source.targetOwner().ownerId(), normalized.ownerId());
        }
        assertEquals(1, authority.commitCount,
            "steady normalization does not re-commit the aggregate");
        coordinator.normalizeCompletedAggregate(
                expected,
                activated.fence(),
                source.targetOwner(),
                NEVER)
            .toCompletableFuture().join();
        assertEquals(1, authority.commitCount,
            "an already steady aggregate is idempotent");
    }

    @Test
    void completedAggregateReconcilesWhenOneParticipantWasAlreadyNormalized() {
        FakeAuthorityStore authority = new FakeAuthorityStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            new FakeRelocationStore());
        var source = request();
        var activated = coordinator.commit(
                coordinator.prepare(source, NEVER)
                    .toCompletableFuture().join(),
                NEVER)
            .toCompletableFuture().join();
        coordinator.completeSourceCleanup(activated, goldenRoot(), NEVER)
            .toCompletableFuture().join();
        var first = authority.rows.get(source.participants().getFirst()
            .authorityKey());
        var publication = ZLinkCanonicalRelocationAuthorityStateCodec.decode(
            first.payload());
        authority.rows.put(source.participants().getFirst().authorityKey(),
            new ZLinkAuthoritySnapshot(
                "already-steady",
                publication.applicationPayload(),
                first.objectGeneration(),
                first.authorityOwnerGeneration(),
                first.ownerId(),
                first.ownerLeaseGeneration(),
                first.allocation(),
                first.storeNow()));
        var expected = source.participants().stream()
            .map(value -> new ZLinkAggregateRelocationCoordinator
                .ExpectedParticipant(
                    value.authorityKey(),
                    value.objectGeneration(),
                    value.authorityOwnerGeneration()))
            .toList();

        assertDoesNotThrow(() -> coordinator.normalizeCompletedAggregate(
                expected,
                activated.fence(),
                source.targetOwner(),
                NEVER)
            .toCompletableFuture().join());
        assertTrue(authority.progress == null,
            "normalization removes the marker after all participants are steady");
    }

    @Test
    void authorityPayloadPublishesCanonicalRelocationSlot() {
        FakeAuthorityStore authority = new FakeAuthorityStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority, new FakeRelocationStore());
        coordinator.prepare(request(), NEVER).toCompletableFuture().join();

        var decoded = ZLinkCanonicalRelocationAuthorityStateCodec.decode(
            authority.prepared.participants().getFirst().authorityPayload());

        assertNotNull(decoded);
        assertEquals(new UUID(0, 9), decoded.aggregateId());
        assertEquals(7, decoded.aggregateGeneration());
        assertEquals("owner-b", decoded.targetOwnerId());

        for (var participant : authority.prepared.participants()) {
            var participantDecoded = ZLinkCanonicalRelocationAuthorityStateCodec
                .decode(participant.authorityPayload());
            assertNotNull(participantDecoded);
            if (participant.authorityKey().startsWith("spot:")) {
                var application = new ZLinkServiceAuthorityPayloadCodec()
                    .decode(participantDecoded.applicationPayload())
                    .orElseThrow();
                assertEquals("owner-b", application.ownerId());
                assertEquals(12, application.ownerLeaseGeneration());
                assertEquals(RoutingId.from("node-b"), application.nodeRid());
                assertEquals(4, application.nodeGeneration());
            } else {
                var actor = new ZLinkActorAuthorityPayloadCodec()
                    .decode(participantDecoded.applicationPayload())
                    .orElseThrow();
                assertEquals("owner-b", actor.ownerId());
                assertEquals(12, actor.ownerLeaseGeneration());
                assertEquals(RoutingId.from("node-b"), actor.nodeRid());
                assertEquals(4, actor.nodeGeneration());
            }
        }
    }

    @Test
    void startupScannerFindsAndVerifiesPublishedAggregateOnce() {
        FakeAuthorityStore authority = new FakeAuthorityStore();
        FakeRelocationStore relocation = new FakeRelocationStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority, relocation);
        coordinator.commit(
                coordinator.prepare(request(), NEVER)
                    .toCompletableFuture().join(),
                NEVER)
            .toCompletableFuture().join();

        var candidates = new ZLinkRelocationStartupScanner(
            authority, relocation).scan(NEVER).toCompletableFuture().join();

        assertEquals(1, candidates.size());
        var candidate = candidates.getFirst();
        assertEquals(new UUID(0, 9), candidate.fence().aggregateId());
        assertEquals("owner-a", candidate.sourceOwnerId());
        assertEquals(6, candidate.sourceOwnerLeaseGeneration());
        assertEquals(RoutingId.from("node-a"), candidate.sourceNodeRid());
        assertEquals(3, candidate.sourceNodeGeneration());
        assertEquals("owner-b", candidate.targetOwner().ownerId());
        assertEquals(RoutingId.from("node-b"), candidate.targetNodeRid());
        assertEquals(4, candidate.targetNodeGeneration());
        assertEquals(2, candidate.authorities().size());
        assertFalse(candidate.sourceCleanupCompleted());
        assertArrayEquals(goldenRoot(), candidate.root().payload());
    }

    @Test
    void canonicalReplyEvidenceSurvivesSourceCleanupPublication() {
        FakeAuthorityStore authority = new FakeAuthorityStore();
        FakeRelocationStore relocation = new FakeRelocationStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority, relocation);
        var source = request();
        var activated = coordinator.commit(
                coordinator.prepare(source, NEVER)
                    .toCompletableFuture().join(),
                NEVER)
            .toCompletableFuture().join();
        var expected = source.participants().stream()
            .map(value -> new ZLinkAggregateRelocationCoordinator
                .ExpectedParticipant(
                    value.authorityKey(),
                    value.objectGeneration(),
                    value.authorityOwnerGeneration()))
                .toList();
        var completion = ZLinkServiceRelocationEnvelopeCodec
            .decode(goldenRoot()).terminalCompletions().getFirst();
        int rootsBeforeReplay = relocation.valueCount();

        var durable = coordinator.updateCanonicalReplay(
                expected,
                source.targetOwner(),
                current -> ZLinkServiceRelocationEnvelopeCodec
                    .completeDelivery(
                        current,
                        completion.operationHigh(),
                        completion.operationLow(),
                        completion.sourceOwnerId(),
                        completion.sourceOwnerLeaseGeneration(),
                        RoutingId.from(completion.sourceNodeRid()),
                        completion.sourceNodeGeneration(),
                        2),
                NEVER)
            .toCompletableFuture().join();
        assertEquals(2, durable.root().terminalCompletions().getFirst()
            .deliveryState());
        assertTrue(durable.root().recoveryReleaseEligible());
        assertEquals(rootsBeforeReplay + 2, relocation.valueCount(),
            "the previous root remains available for retention cleanup");
        assertFalse(ZLinkCanonicalRelocationAuthorityStateCodec.decode(
            authority.rows.get("spot:room-a").payload())
            .sourceCleanupCompleted());

        coordinator.completeSourceCleanup(activated, goldenRoot(), NEVER)
            .toCompletableFuture().join();
        coordinator.verifyCompletedAggregate(
                expected,
                activated.fence(),
                source.targetOwner(),
                NEVER)
            .toCompletableFuture().join();
        assertTrue(authority.progress.sourceCleanupCompleted(),
            "cleanup phase is owned by the aggregate marker");
        var published = ZLinkCanonicalRelocationAuthorityStateCodec.decode(
            authority.rows.get("spot:room-a").payload());
        assertEquals("owner-a", published.sourceOwnerId());
        assertEquals("owner-b", published.targetOwnerId());
        var application = new ZLinkServiceAuthorityPayloadCodec()
            .decode(published.applicationPayload())
            .orElseThrow();
        assertEquals("owner-b", application.ownerId());
        assertEquals(12, application.ownerLeaseGeneration());
        assertEquals(
            source.participants().getFirst().authorityOwnerGeneration() + 1,
            authority.rows.get("spot:room-a").authorityOwnerGeneration());
    }

    @Test
    void publishedAggregateRejectsDifferentParticipantGenerationFence() {
        FakeAuthorityStore authority = new FakeAuthorityStore();
        FakeRelocationStore relocation = new FakeRelocationStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var source = request();
        var published = coordinator.commit(
                coordinator.prepare(source, NEVER)
                    .toCompletableFuture().join(),
                NEVER)
            .toCompletableFuture().join();
        List<ZLinkAggregateRelocationCoordinator.ExpectedParticipant>
            expected = source.participants().stream()
                .map(value -> new ZLinkAggregateRelocationCoordinator
                    .ExpectedParticipant(
                        value.authorityKey(),
                        value.objectGeneration(),
                        value.authorityOwnerGeneration()))
                .toList();
        expected = new java.util.ArrayList<>(expected);
        var first = expected.getFirst();
        expected.set(0, new ZLinkAggregateRelocationCoordinator
            .ExpectedParticipant(
                first.authorityKey(),
                first.objectGeneration(),
                first.sourceAuthorityOwnerGeneration() + 1));

        var exact = expected;
        var failure = assertThrows(
            java.util.concurrent.CompletionException.class,
            () -> coordinator.readPublishedAggregate(
                    exact,
                    published.fence(),
                    source.targetOwner(),
                    published.inventoryDigest(),
                    NEVER)
                .toCompletableFuture().join());
        assertInstanceOf(
            ZLinkAggregateRelocationCoordinator.RelocationDataLostException.class,
            failure.getCause());
    }

    @Test
    void publishedAggregateUsesProviderGenerationWhenUnrelatedOwnersAdvanceCounter() {
        FakeAuthorityStore authority = new FakeAuthorityStore();
        authority.ownerGenerationGap = 3;
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            new FakeRelocationStore());
        var source = request();
        var published = coordinator.commit(
                coordinator.prepare(source, NEVER)
                    .toCompletableFuture().join(),
                NEVER)
            .toCompletableFuture().join();

        assertEquals(8L, published.targetOwnerGeneration("spot:room-a"));
        assertEquals(8L, published.targetOwnerGeneration("actor:user-a"));
        var expected = source.participants().stream()
            .map(value -> new ZLinkAggregateRelocationCoordinator
                .ExpectedParticipant(
                    value.authorityKey(),
                    value.objectGeneration(),
                    value.authorityOwnerGeneration()))
            .toList();

        var root = coordinator.readPublishedAggregate(
                expected,
                published.fence(),
                source.targetOwner(),
                published.inventoryDigest(),
                NEVER)
            .toCompletableFuture().join();
        assertEquals(8L, root.targetOwnerGeneration("spot:room-a"));
        assertEquals(8L, root.targetOwnerGeneration("actor:user-a"));
    }

    @Test
    void treeReadRejectsCorruptChunkBeforeTargetStaging() {
        FakeRelocationStore relocation = new FakeRelocationStore();
        byte[] logicalRoot = new byte[] {9, 8, 7, 6};
        byte[] inventoryDigest = new byte[32];
        var stored = ZLinkRelocationTreeStore.put(
                relocation,
                logicalRoot,
                inventoryDigest,
                Duration.ofHours(24),
                NEVER)
            .toCompletableFuture().join();
        var read = ZLinkRelocationTreeStore.read(
                relocation,
                stored.root().reference(),
                stored.root().checksumCrc32c(),
                NEVER)
            .toCompletableFuture().join();
        assertArrayEquals(logicalRoot, read.logicalRoot());
        assertArrayEquals(inventoryDigest, read.inventoryDigest());

        relocation.corruptChunk();
        var failure = assertThrows(
            java.util.concurrent.CompletionException.class,
            () -> ZLinkRelocationTreeStore.read(
                    relocation,
                    stored.root().reference(),
                    stored.root().checksumCrc32c(),
                    NEVER)
                .toCompletableFuture().join());
        assertInstanceOf(
            ZLinkRelocationTreeStore.DataLostException.class,
            failure.getCause());
    }

    @Test
    void treeSplitsAtSixtyFourMiBAndRenewsEveryComponent() {
        FakeRelocationStore relocation = new FakeRelocationStore();
        byte[] logicalRoot = new byte[ZLinkRelocationTreeStore.CHUNK_BYTES + 1];
        logicalRoot[0] = 1;
        logicalRoot[logicalRoot.length - 1] = 2;
        byte[] inventoryDigest = new byte[32];

        var stored = ZLinkRelocationTreeStore.put(
                relocation,
                logicalRoot,
                inventoryDigest,
                Duration.ofHours(24),
                NEVER)
            .toCompletableFuture().join();
        assertEquals(2, stored.chunkCount());
        var read = ZLinkRelocationTreeStore.read(
                relocation,
                stored.root().reference(),
                stored.root().checksumCrc32c(),
                NEVER)
            .toCompletableFuture().join();
        assertArrayEquals(logicalRoot, read.logicalRoot());
        assertEquals(2, read.chunkCount());

        ZLinkRelocationTreeStore.renew(
                relocation,
                stored.root().reference(),
                stored.root().checksumCrc32c(),
                Duration.ofHours(24),
                NEVER)
            .toCompletableFuture().join();
        assertEquals(3, relocation.renewCount,
            "both chunks and the manifest renew retention");
    }

    @Test
    void deletingOneManifestPreservesItsSharedContentAddressedChunk() {
        FakeRelocationStore relocation = new FakeRelocationStore();
        byte[] logicalRoot = new byte[] {9, 8, 7, 6};
        byte[] firstDigest = new byte[32];
        byte[] secondDigest = new byte[32];
        secondDigest[0] = 1;
        var first = ZLinkRelocationTreeStore.put(
                relocation,
                logicalRoot,
                firstDigest,
                Duration.ofHours(24),
                NEVER)
            .toCompletableFuture().join();
        var second = ZLinkRelocationTreeStore.put(
                relocation,
                logicalRoot,
                secondDigest,
                Duration.ofHours(24),
                NEVER)
            .toCompletableFuture().join();
        assertEquals(3, relocation.valueCount(),
            "the two manifests share one content-addressed chunk");

        ZLinkRelocationTreeStore.delete(
                relocation,
                first.root().reference(),
                NEVER)
            .toCompletableFuture().join();

        assertEquals(2, relocation.valueCount());
        var surviving = ZLinkRelocationTreeStore.read(
                relocation,
                second.root().reference(),
                second.root().checksumCrc32c(),
                NEVER)
            .toCompletableFuture().join();
        assertArrayEquals(logicalRoot, surviving.logicalRoot());
        assertArrayEquals(secondDigest, surviving.inventoryDigest());
    }

    private static ZLinkAggregateRelocationCoordinator.Request request() {
        return new ZLinkAggregateRelocationCoordinator.Request(
            new UUID(0, 9),
            7,
            List.of(
                participant("spot:room-a", ZLinkPlacementObjectKind.USER_SPOT),
                participant("actor:user-a", ZLinkPlacementObjectKind.ACTOR)),
            goldenRoot(),
            new ZLinkMeshNodeDescriptorKey(
                "game",
                RoutingId.from("node-b")),
            4,
            new ZLinkPlacementCapacityBundle(
                1,
                1,
                Optional.of(new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    "RoomSpot",
                    1))),
            new ZLinkLocationOwnerToken("owner-b", 12));
    }

    private static ZLinkAggregateRelocationCoordinator.Participant participant(
        String key,
        ZLinkPlacementObjectKind kind) {
        return new ZLinkAggregateRelocationCoordinator.Participant(
            key,
            kind,
            3,
            5,
            "version-1",
            ZLinkAuthorityGenerationTransition.NEW_OWNER,
            authorityPayload(kind),
            new byte[] {2});
    }

    private static byte[] authorityPayload(ZLinkPlacementObjectKind kind) {
        RoutingId node = RoutingId.from("node-a");
        if (kind == ZLinkPlacementObjectKind.ACTOR) {
            return new ZLinkActorAuthorityPayloadCodec().encode(
                ZLinkActorAuthorityPayloadCodec.State.READY,
                "Player", "user-a", "room-a", 3, 2,
                "owner-a", 6, "game", node, 3);
        }
        return new ZLinkServiceAuthorityPayloadCodec().encodeUser(
            ZLinkServiceAuthorityPayloadCodec.State.READY,
            "RoomSpot", "room-a", "owner-a", 6, "game", node, 3);
    }

    private static final Pattern LOGICAL_HEX = Pattern.compile(
        "\\\"logicalHex\\\"\\s*:\\s*\\\"([0-9a-f]+)\\\"");

    private static byte[] goldenRoot() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path fixture = current.resolve(
                "runtime/protocol/golden/relocation-envelope-v1.json");
            if (Files.isRegularFile(fixture)) {
                try {
                    var match = LOGICAL_HEX.matcher(Files.readString(fixture));
                    if (match.find()) return HexFormat.of().parseHex(match.group(1));
                } catch (java.io.IOException failure) {
                    throw new IllegalStateException(failure);
                }
            }
            current = current.getParent();
        }
        throw new IllegalStateException("shared relocation fixture was not found");
    }

    private static final class FakeRelocationStore
        implements ZLinkRelocationStore {
        private final Map<String, byte[]> values = new ConcurrentHashMap<>();
        private int renewCount;
        private int deleteCount;

        @Override
        public CompletionStage<ZLinkRelocationStored> put(
            byte[] payload,
            Duration retention,
            ZLinkStoreCancellation cancellation) {
            String reference = "sha256:" + sha256(payload);
            values.put(reference, payload.clone());
            return CompletableFuture.completedFuture(new ZLinkRelocationStored(
                reference,
                checksum(payload),
                Instant.now().plus(retention),
                Instant.now()));
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
            renewCount++;
            return CompletableFuture.completedFuture(
                new ZLinkRelocationRenewed(
                    Instant.now().plus(retention),
                    Instant.now()));
        }

        @Override
        public CompletionStage<ZLinkRelocationDeleteResult> delete(
            String reference,
            ZLinkStoreCancellation cancellation) {
            deleteCount++;
            return CompletableFuture.completedFuture(
                values.remove(reference) == null
                    ? ZLinkRelocationDeleteResult.MISSING
                    : ZLinkRelocationDeleteResult.DELETED);
        }

        private static long checksum(byte[] payload) {
            CRC32C checksum = new CRC32C();
            checksum.update(payload);
            return checksum.getValue();
        }

        private static String sha256(byte[] payload) {
            try {
                return HexFormat.of().formatHex(
                    MessageDigest.getInstance("SHA-256").digest(payload));
            } catch (NoSuchAlgorithmException failure) {
                throw new AssertionError(failure);
            }
        }

        private int valueCount() {
            return values.size();
        }

        private void corruptChunk() {
            values.replaceAll((reference, bytes) -> {
                byte[] copy = bytes.clone();
                if (copy.length >= 4 && copy[0] == 'Z' && copy[3] == 'C') {
                    copy[copy.length - 1] ^= 1;
                }
                return copy;
            });
        }
    }

    private static final class FakeAuthorityStore extends ZLinkLocationStoreTestAdapter {
        private ZLinkAggregatePrepareRequest prepared;
        private ZLinkAggregatePrepareResult prepareResult;
        private int prepareCount;
        private int commitCount;
        private int abortCount;
        private long ownerGenerationGap = 1;
        private ZLinkAggregateProgress progress;
        private String progressStoreVersion;
        private final Map<String, ZLinkAuthoritySnapshot> rows =
            new ConcurrentHashMap<>();

        @Override
        public CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
            ZLinkAggregatePrepareRequest request,
            ZLinkStoreCancellation cancellation) {
            prepareCount++;
            prepared = request;
            return CompletableFuture.completedFuture(prepareResult != null
                ? prepareResult
                : new ZLinkAggregatePrepared(new ZLinkAggregateFence(
                    request.aggregateId(),
                    request.aggregateGeneration())));
        }

        @Override
        public CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation) {
            commitCount++;
            for (ZLinkAggregateParticipant participant :
                prepared.participants()) {
                ZLinkAggregateRelocationCoordinator.Participant source =
                    request().participants().stream()
                        .filter(value -> value.authorityKey().equals(
                            participant.authorityKey()))
                        .findFirst()
                        .orElseThrow();
                ZLinkPlacementCapacityBundle capacity =
                    source.objectKind() == ZLinkPlacementObjectKind.ACTOR
                        ? ZLinkPlacementCapacityBundle.actor(1)
                        : ZLinkPlacementCapacityBundle.spot(
                            source.objectKind(), "RoomSpot", 1);
                rows.put(participant.authorityKey(),
                    new ZLinkAuthoritySnapshot(
                        "committed-" + commitCount + "-"
                            + participant.authorityKey(),
                        participant.authorityPayload(),
                        source.objectGeneration(),
                        source.authorityOwnerGeneration() + ownerGenerationGap,
                        prepared.targetOwner().ownerId(),
                        prepared.targetOwner().leaseGeneration(),
                    new ZLinkPlacementAllocation(
                            ZLinkPlacementAllocationState.ACTIVE,
                            source.objectKind(),
                            "RoomSpot",
                            prepared.targetDescriptor(),
                            prepared.targetDescriptorLifecycleGeneration(),
                            capacity),
                        Instant.now()));
            }
            progress = ZLinkCanonicalRelocationAuthorityStateCodec.progress(
                prepared.participants().getFirst().authorityPayload());
            progressStoreVersion = "aggregate-commit-" + commitCount;
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
            progressStoreVersion = "aggregate-progress-"
                + progressStoreVersion;
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
        public CompletionStage<ZLinkAggregateAbortResult> abortAggregate(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation) {
            abortCount++;
            return CompletableFuture.completedFuture(
                ZLinkAggregateAbortResult.ABORTED);
        }

        @Override
        public CompletionStage<ZLinkAuthorityReadResult> read(
            String key, ZLinkStoreCancellation cancellation) {
            ZLinkAuthoritySnapshot row = rows.get(key);
            return CompletableFuture.completedFuture(row == null
                ? new ZLinkAuthorityMissing(Instant.now())
                : row);
        }

        @Override
        public CompletionStage<ZLinkAuthorityScanResult> list(
            String prefix,
            Optional<ZLinkAuthorityScanCursor> cursor,
            int limit,
            ZLinkStoreCancellation cancellation) {
            ZLinkPlacementObjectKind expected = prefix.endsWith("a:")
                ? ZLinkPlacementObjectKind.ACTOR
                : ZLinkPlacementObjectKind.USER_SPOT;
            List<ZLinkAuthorityEntry> entries = rows.entrySet().stream()
                .filter(value -> value.getValue().allocation().objectKind()
                    == expected)
                .map(value -> new ZLinkAuthorityEntry(
                    value.getKey(), value.getValue()))
                .sorted(java.util.Comparator.comparing(
                    ZLinkAuthorityEntry::key))
                .toList();
            return CompletableFuture.completedFuture(
                new ZLinkAuthorityPage(entries, Optional.empty()));
        }

        @Override
        public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
            String key, ZLinkAuthorityExpectation expectation,
            ZLinkAuthorityMutation mutation, ZLinkStoreCancellation cancellation) {
            ZLinkAuthoritySnapshot current = rows.get(key);
            if (!(expectation instanceof ZLinkAuthorityExpectFound found)
                || current == null
                || !current.storeVersion().equals(found.storeVersion())
                || !(mutation instanceof ZLinkAuthorityPut put)
                || put.generationTransition()
                    != ZLinkAuthorityGenerationTransition.PRESERVE) {
                return CompletableFuture.completedFuture(
                    new ZLinkAuthorityConflict(current == null
                        ? new ZLinkAuthorityMissing(Instant.now())
                        : current));
            }
            ZLinkAuthoritySnapshot stored = new ZLinkAuthoritySnapshot(
                "normalized-" + key,
                put.payload(),
                current.objectGeneration(),
                current.authorityOwnerGeneration(),
                current.ownerId(),
                current.ownerLeaseGeneration(),
                current.allocation(),
                Instant.now());
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

        @Override
        public CompletionStage<ZLinkObjectReserveResult> reserve(
            ZLinkObjectReservationRequest request,
            ZLinkStoreCancellation cancellation) {
            return unsupported();
        }

        @Override
        public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation, byte[] payload,
            ZLinkStoreCancellation cancellation) {
            return unsupported();
        }

        @Override
        public CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation, byte[] payload,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
            return unsupported();
        }

        @Override
        public CompletionStage<ZLinkObjectRejectResult> reject(
            ZLinkObjectReservation reservation,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
            return unsupported();
        }

        @Override
        public CompletionStage<ZLinkObjectAbortResult> abort(
            ZLinkObjectReservation reservation,
            ZLinkStoreCancellation cancellation) {
            return unsupported();
        }

        @Override
        public CompletionStage<ZLinkObjectAbortResult> abort(
            ZLinkObjectReservation reservation,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
            return unsupported();
        }

        @Override
        public CompletionStage<ZLinkCreationTerminalReadResult>
            readCreationTerminal(
                ZLinkCreationOperationIdentity operation,
                ZLinkStoreCancellation cancellation) {
            return unsupported();
        }

        @Override
        public CompletionStage<ZLinkRelocationCapacityReserveResult>
            reserveRelocationCapacity(
                ZLinkRelocationCapacityReservationRequest request,
                ZLinkStoreCancellation cancellation) {
            return unsupported();
        }

        @Override
        public CompletionStage<ZLinkRelocationCapacityAbortResult>
            abortRelocationCapacity(
                ZLinkRelocationCapacityFence fence,
                ZLinkStoreCancellation cancellation) {
            return unsupported();
        }

        private static <T> CompletionStage<T> unsupported() {
            return CompletableFuture.failedFuture(
                new UnsupportedOperationException());
        }
    }
}
