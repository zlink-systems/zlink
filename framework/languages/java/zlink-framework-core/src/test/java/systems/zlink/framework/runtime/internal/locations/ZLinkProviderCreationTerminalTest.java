package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreMissingCondition;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadMissing;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteConflict;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

final class ZLinkProviderCreationTerminalTest {
    private static final ZLinkCreationOperationIdentity OPERATION =
            new ZLinkCreationOperationIdentity(
                    RoutingId.from(new byte[] {0, (byte) 0xff, 0x10}), 7L, 1L, 0xabcdefL);
    private static final Duration ORIGINAL_DEADLINE = Duration.ofSeconds(17);
    private static final Duration RETENTION = Duration.ofMinutes(5);

    @ParameterizedTest
    @EnumSource(ZLinkCreationTerminalState.class)
    void publishesAuthorityCapacityAndTerminalTogetherAndReplaysFromStore(
            ZLinkCreationTerminalState state) {
        var fixture = new Fixture();
        var terminal = fixture.terminal(state);

        assertEquals(applied(state), fixture.complete(terminal));
        assertEquals(1, fixture.provider.writes.size());
        var write = fixture.provider.writes.getFirst();
        assertEquals(3, write.mutations().size());
        var terminalPuts = fixture.provider.terminalPuts();
        assertEquals(1, terminalPuts.size());
        var terminalPut = terminalPuts.getFirst();
        assertEquals(
                "creation-terminal\0" + "00ff10\0" + "7\0" + "00000000000000010000000000abcdef",
                terminalPut.key().value());
        assertTrue(write.conditions().contains(new ZLinkStoreMissingCondition(terminalPut.key())));
        assertEquals(ORIGINAL_DEADLINE.plus(RETENTION), terminalPut.retention());
        assertArrayEquals(terminal.terminalEnvelope(), terminalPut.bytes());

        var replay = new ZLinkProviderLocationRepository(fixture.provider);
        assertTerminal(terminal, await(replay.readCreationTerminal(OPERATION, () -> false)));
        if (state == ZLinkCreationTerminalState.CREATED) {
            var authority =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            await(replay.read(fixture.reservation.authorityKey(), () -> false)));
            assertArrayEquals(new byte[] {9}, authority.payload());
            assertEquals(ZLinkPlacementAllocationState.ACTIVE, authority.allocation().state());
            assertTrue(authority.pendingCreation().isEmpty());
            assertInstanceOf(
                    ZLinkPlacementCapacityExhausted.class,
                    await(replay.reserve(fixture.request("next"), () -> false)));
        } else {
            assertInstanceOf(
                    ZLinkAuthorityMissing.class,
                    await(replay.read(fixture.reservation.authorityKey(), () -> false)));
            assertInstanceOf(
                    ZLinkObjectReserved.class,
                    await(replay.reserve(fixture.request("next"), () -> false)));
        }
    }

    @Test
    void readsNodeProductionSchemaTerminalBytesFromTheCanonicalKey() {
        var fixture = new Fixture();
        byte[] nodeTerminal =
                java.util.HexFormat.of()
                        .parseHex(
                                "01000000250000000000000000010200180f6163746f722d63616e6f6e6963616c"
                                        + "000000000000000100");
        var key =
                new ZLinkStoreKey(
                        "creation-terminal\0"
                                + "00ff10\0"
                                + "7\0"
                                + "00000000000000010000000000abcdef");
        await(
                fixture.provider.delegate.write(
                        new ZLinkStoreWriteRequest(
                                List.of(), List.of(new ZLinkStorePut(key, nodeTerminal, null))),
                        () -> false));

        byte[] stored =
                assertInstanceOf(
                                ZLinkCreationTerminalFound.class,
                                await(
                                        fixture.repository.readCreationTerminal(
                                                OPERATION, () -> false)))
                        .terminalEnvelope();
        assertArrayEquals(nodeTerminal, stored);
        var codec = new ZLinkServiceM6BWireCodec();
        assertArrayEquals(
                nodeTerminal,
                codec.encodeCreationOperationTerminal(
                        new ZLinkServiceM6BWireCodec.ActorCreationTerminal(
                                0,
                                0,
                                new ZLinkServiceM6BWireCodec.ActorCreateTerminal(
                                        ZLinkServiceM6BWireCodec.ActorCreateResult.CREATED,
                                        new ActorRef(
                                                "actor-canonical",
                                                1L,
                                                "canonical-mesh",
                                                RoutingId.from("canonical-node"))),
                                null)));
        var decoded =
                codec.decodeCreationOperationTerminal(
                        stored, "canonical-mesh", RoutingId.from("canonical-node"));
        assertEquals("actor-canonical", decoded.creation().actor().actorId());
        assertEquals(1L, decoded.creation().actor().objectGeneration());
    }

    @ParameterizedTest
    @EnumSource(ZLinkCreationTerminalState.class)
    void conditionalConflictPublishesNeitherTerminalNorStateTransition(
            ZLinkCreationTerminalState state) {
        var fixture = new Fixture();
        fixture.provider.conflictNextWrite = true;

        assertEquals(stale(state), fixture.complete(fixture.terminal(state)));
        assertEquals(1, fixture.provider.writes.size());
        assertInstanceOf(
                ZLinkCreationTerminalMissing.class,
                await(fixture.repository.readCreationTerminal(OPERATION, () -> false)));
        var authority =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        await(
                                fixture.repository.read(
                                        fixture.reservation.authorityKey(), () -> false)));
        assertEquals(fixture.reservation.storeVersion(), authority.storeVersion());
        assertEquals(ZLinkPlacementAllocationState.PENDING, authority.allocation().state());
        assertTrue(authority.pendingCreation().isPresent());
        assertInstanceOf(
                ZLinkPlacementCapacityExhausted.class,
                await(fixture.repository.reserve(fixture.request("next"), () -> false)));
    }

    @Test
    void completionRejectsReservationWithStaleAuthorityStoreVersion() {
        var fixture = new Fixture();
        var reservation = fixture.reservation;
        var stale =
                new ZLinkObjectReservation(
                        reservation.authorityKey(),
                        "stale-store-version",
                        reservation.objectGeneration(),
                        reservation.authorityOwnerGeneration(),
                        reservation.reservationVersion(),
                        reservation.targetDescriptor(),
                        reservation.targetDescriptorLifecycleGeneration(),
                        reservation.targetOwner());

        assertEquals(
                ZLinkObjectCommitResult.STALE,
                await(fixture.repository.commit(stale, new byte[] {9}, null, () -> false)));
        assertTrue(fixture.provider.writes.isEmpty());
        var authority =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        await(fixture.repository.read(reservation.authorityKey(), () -> false)));
        assertEquals(reservation.storeVersion(), authority.storeVersion());
        assertEquals(ZLinkPlacementAllocationState.PENDING, authority.allocation().state());
        assertTrue(authority.pendingCreation().isPresent());
    }

    @ParameterizedTest
    @ValueSource(ints = {0, 1, 2, 3})
    void identityMismatchIsMissingEvenWhenProviderReturnsAnotherOperationsRecord(int changedField) {
        var fixture = new Fixture();
        var terminal = fixture.terminal(ZLinkCreationTerminalState.FAILED);
        assertEquals(ZLinkObjectAbortResult.ABORTED, fixture.complete(terminal));
        var stored = fixture.provider.terminalPuts().getFirst();
        var other =
                new ZLinkCreationOperationIdentity(
                        changedField == 0 ? RoutingId.from("other") : OPERATION.sourceNodeRid(),
                        changedField == 1 ? 1 : OPERATION.sourceLifecycleGeneration(),
                        changedField == 2 ? 2 : OPERATION.operationIdHigh(),
                        changedField == 3 ? 1 : OPERATION.operationIdLow());

        assertInstanceOf(
                ZLinkCreationTerminalMissing.class,
                await(fixture.repository.readCreationTerminal(other, () -> false)));
        fixture.reservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                await(
                                        fixture.repository.reserve(
                                                fixture.request("actor"), () -> false)))
                        .reservation();
        fixture.provider.writes.clear();
        var otherTerminal =
                new ZLinkCreationOperationTerminal(
                        other,
                        fixture.reservation,
                        terminal.state(),
                        terminal.terminalEnvelope(),
                        terminal.expiresAt());
        assertEquals(ZLinkObjectAbortResult.ABORTED, fixture.complete(otherTerminal));
        var otherKey = fixture.provider.terminalPuts().getFirst().key();
        await(
                fixture.provider.delegate.write(
                        new ZLinkStoreWriteRequest(
                                List.of(),
                                List.of(new ZLinkStorePut(otherKey, stored.bytes(), null))),
                        () -> false));
        assertArrayEquals(
                terminal.terminalEnvelope(),
                assertInstanceOf(
                                ZLinkCreationTerminalFound.class,
                                await(fixture.repository.readCreationTerminal(other, () -> false)))
                        .terminalEnvelope());
        assertTerminal(
                terminal, await(fixture.repository.readCreationTerminal(OPERATION, () -> false)));
    }

    @ParameterizedTest
    @EnumSource(ZLinkCreationTerminalState.class)
    void retentionEndsAtOriginalDeadlinePlusFiveMinutes(ZLinkCreationTerminalState state) {
        var fixture = new Fixture();
        var terminal = fixture.terminal(state);
        fixture.clock.advance(Duration.ofSeconds(7));
        assertEquals(applied(state), fixture.complete(terminal));
        var terminalPuts = fixture.provider.terminalPuts();
        var stored = terminalPuts.getFirst();
        assertEquals(Duration.ofSeconds(10).plus(RETENTION), stored.retention());

        fixture.clock.advance(stored.retention().minusMillis(1));
        assertTerminal(
                terminal, await(fixture.repository.readCreationTerminal(OPERATION, () -> false)));
        fixture.clock.advance(Duration.ofMillis(1));
        for (var put : terminalPuts) {
            assertInstanceOf(
                    ZLinkStoreReadMissing.class,
                    await(fixture.provider.read(put.key(), () -> false)));
        }
        assertInstanceOf(
                ZLinkCreationTerminalMissing.class,
                await(fixture.repository.readCreationTerminal(OPERATION, () -> false)));

        // A provider may retain expired bytes until its cleanup runs.
        for (var put : terminalPuts) {
            await(
                    fixture.provider.delegate.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(new ZLinkStorePut(put.key(), put.bytes(), null))),
                            () -> false));
        }
        assertArrayEquals(
                terminal.terminalEnvelope(),
                assertInstanceOf(
                                ZLinkCreationTerminalFound.class,
                                await(
                                        fixture.repository.readCreationTerminal(
                                                OPERATION, () -> false)))
                        .terminalEnvelope());
    }

    @Test
    void recoveryAbortHasNoTerminalWhileFailureAbortPublishesFailed() {
        var fixture = new Fixture();
        assertEquals(
                ZLinkObjectAbortResult.ABORTED,
                await(fixture.repository.abort(fixture.reservation, () -> false)));
        assertEquals(1, fixture.provider.writes.size());
        assertEquals(2, fixture.provider.writes.getFirst().mutations().size());
        assertInstanceOf(
                ZLinkCreationTerminalMissing.class,
                await(fixture.repository.readCreationTerminal(OPERATION, () -> false)));

        fixture.reservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                await(
                                        fixture.repository.reserve(
                                                fixture.request("actor"), () -> false)))
                        .reservation();
        fixture.provider.writes.clear();
        var failed = fixture.terminal(ZLinkCreationTerminalState.FAILED);
        assertEquals(ZLinkObjectAbortResult.ABORTED, fixture.complete(failed));
        assertEquals(1, fixture.provider.writes.size());
        assertEquals(3, fixture.provider.writes.getFirst().mutations().size());
        assertTerminal(
                failed, await(fixture.repository.readCreationTerminal(OPERATION, () -> false)));
    }

    @Test
    void existingTerminalCannotBeOverwrittenByAnotherReservation() {
        var fixture = new Fixture();
        var original = fixture.terminal(ZLinkCreationTerminalState.FAILED);
        assertEquals(ZLinkObjectAbortResult.ABORTED, fixture.complete(original));
        fixture.reservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                await(
                                        fixture.repository.reserve(
                                                fixture.request("actor"), () -> false)))
                        .reservation();
        fixture.provider.writes.clear();

        assertEquals(
                ZLinkObjectCommitResult.STALE,
                fixture.complete(fixture.terminal(ZLinkCreationTerminalState.CREATED)));
        assertEquals(1, fixture.provider.writes.size());
        assertTerminal(
                original, await(fixture.repository.readCreationTerminal(OPERATION, () -> false)));
        var authority =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        await(
                                fixture.repository.read(
                                        fixture.reservation.authorityKey(), () -> false)));
        assertEquals(fixture.reservation.storeVersion(), authority.storeVersion());
    }

    @Test
    void productionCreationTerminalKeyMatchesGoldenVector() throws Exception {
        var fixture = new Fixture();
        var operation =
                new ZLinkCreationOperationIdentity(RoutingId.fromHex("01020304"), 7L, 0x2aL, 1L);
        var original = fixture.terminal(ZLinkCreationTerminalState.CREATED);
        var terminal =
                new ZLinkCreationOperationTerminal(
                        operation,
                        original.reservation(),
                        original.state(),
                        original.terminalEnvelope(),
                        original.expiresAt());

        assertEquals(ZLinkObjectCommitResult.COMMITTED, fixture.complete(terminal));
        String preimage = fixture.provider.terminalPuts().getFirst().key().value();
        JsonNode vector = null;
        JsonNode fixtureJson = new ObjectMapper().readTree(Files.readString(sharedFixturePath()));
        for (JsonNode candidate : fixtureJson.path("keyDerivation")) {
            if ("creation-terminal".equals(candidate.path("record").asText())) {
                vector = candidate;
                break;
            }
        }
        assertTrue(vector != null, "creation-terminal golden vector missing");
        assertEquals(vector.path("preimagePrintable").asText().replace("\\u0000", "\0"), preimage);
        assertEquals(
                vector.path("preimageHex").asText(),
                java.util.HexFormat.of().formatHex(preimage.getBytes(StandardCharsets.UTF_8)));
        assertEquals(
                vector.path("sha256Hex").asText(),
                java.util.HexFormat.of()
                        .formatHex(
                                MessageDigest.getInstance("SHA-256")
                                        .digest(preimage.getBytes(StandardCharsets.UTF_8))));
    }

    @ParameterizedTest
    @EnumSource(ZLinkCreationTerminalState.class)
    void alreadyExpiredTerminalCannotPublishUsingLocalWallTime(ZLinkCreationTerminalState state) {
        var fixture = new Fixture();
        var terminal = fixture.terminal(state);
        fixture.clock.advance(ORIGINAL_DEADLINE.plus(RETENTION));

        var failure = assertThrows(CompletionException.class, () -> fixture.complete(terminal));
        assertInstanceOf(IllegalArgumentException.class, failure.getCause());
        assertEquals(
                "terminal expiresAt must be later than provider store time",
                failure.getCause().getMessage());
        assertTrue(fixture.provider.writes.isEmpty());
        assertInstanceOf(
                ZLinkCreationTerminalMissing.class,
                await(fixture.repository.readCreationTerminal(OPERATION, () -> false)));
    }

    @Test
    void terminalMustMatchItsReservationAndCompletionState() {
        var fixture = new Fixture();
        var created = fixture.terminal(ZLinkCreationTerminalState.CREATED);
        var failed = fixture.terminal(ZLinkCreationTerminalState.FAILED);

        assertThrows(
                IllegalArgumentException.class,
                () ->
                        await(
                                fixture.repository.commit(
                                        fixture.reservation, new byte[] {9}, failed, () -> false)));
        assertThrows(
                IllegalArgumentException.class,
                () -> await(fixture.repository.reject(fixture.reservation, created, () -> false)));
        assertThrows(
                IllegalArgumentException.class,
                () -> await(fixture.repository.abort(fixture.reservation, created, () -> false)));
        var reservation = fixture.reservation;
        var anotherReservation =
                new ZLinkObjectReservation(
                        reservation.authorityKey(),
                        reservation.storeVersion(),
                        reservation.objectGeneration(),
                        reservation.authorityOwnerGeneration(),
                        "another-reservation",
                        reservation.targetDescriptor(),
                        reservation.targetDescriptorLifecycleGeneration(),
                        reservation.targetOwner());
        assertThrows(
                IllegalArgumentException.class,
                () ->
                        await(
                                fixture.repository.commit(
                                        anotherReservation, new byte[] {9}, created, () -> false)));
        assertTrue(fixture.provider.writes.isEmpty());
    }

    @ParameterizedTest
    @EnumSource(ZLinkCreationTerminalState.class)
    void maximumEnvelopeFitsProviderValueLimit(ZLinkCreationTerminalState state) {
        var fixture = new Fixture();
        var original = fixture.terminal(state);
        byte[] envelope = new byte[ZLinkCreationOperationTerminal.MAX_REPLY_ENVELOPE_SIZE];
        Arrays.fill(envelope, (byte) 0x5a);
        var terminal =
                new ZLinkCreationOperationTerminal(
                        OPERATION, fixture.reservation, state, envelope, original.expiresAt());

        assertEquals(applied(state), fixture.complete(terminal));
        assertEquals(1, fixture.provider.writes.size());
        assertTerminal(
                terminal, await(fixture.repository.readCreationTerminal(OPERATION, () -> false)));
    }

    @Test
    void readsStoredEnvelopeBytesAndMissingEnvelopeIsMissing() {
        var fixture = new Fixture();
        var terminal = fixture.terminal(ZLinkCreationTerminalState.CREATED);
        assertEquals(ZLinkObjectCommitResult.COMMITTED, fixture.complete(terminal));
        var payload = fixture.provider.terminalPuts().getLast();
        byte[] corrupted = payload.bytes();
        corrupted[0] ^= 1;
        await(
                fixture.provider.delegate.write(
                        new ZLinkStoreWriteRequest(
                                List.of(),
                                List.of(new ZLinkStorePut(payload.key(), corrupted, null))),
                        () -> false));

        assertArrayEquals(
                corrupted,
                assertInstanceOf(
                                ZLinkCreationTerminalFound.class,
                                await(
                                        fixture.repository.readCreationTerminal(
                                                OPERATION, () -> false)))
                        .terminalEnvelope());

        await(
                fixture.provider.delegate.write(
                        new ZLinkStoreWriteRequest(
                                List.of(), List.of(new ZLinkStoreDelete(payload.key()))),
                        () -> false));
        assertInstanceOf(
                ZLinkCreationTerminalMissing.class,
                await(fixture.repository.readCreationTerminal(OPERATION, () -> false)));
    }

    private static void assertTerminal(
            ZLinkCreationOperationTerminal expected, ZLinkCreationTerminalReadResult read) {
        var actual = assertInstanceOf(ZLinkCreationTerminalFound.class, read);
        assertArrayEquals(expected.terminalEnvelope(), actual.terminalEnvelope());
    }

    private static Object applied(ZLinkCreationTerminalState state) {
        return switch (state) {
            case CREATED -> ZLinkObjectCommitResult.COMMITTED;
            case REJECTED -> ZLinkObjectRejectResult.REJECTED;
            case FAILED -> ZLinkObjectAbortResult.ABORTED;
        };
    }

    private static Object stale(ZLinkCreationTerminalState state) {
        return switch (state) {
            case CREATED -> ZLinkObjectCommitResult.STALE;
            case REJECTED -> ZLinkObjectRejectResult.STALE;
            case FAILED -> ZLinkObjectAbortResult.STALE;
        };
    }

    private static Path sharedFixturePath() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve("runtime/protocol/golden/store-record-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException("shared store record golden fixture was not found");
    }

    private static <T> T await(CompletionStage<T> stage) {
        return stage.toCompletableFuture().join();
    }

    private static final class Fixture {
        final StoreClock clock = new StoreClock();
        final RecordingProvider provider = new RecordingProvider(clock);
        final ZLinkProviderLocationRepository repository =
                new ZLinkProviderLocationRepository(provider);
        final ZLinkLocationOwnerToken owner;
        final ZLinkMeshNodeDescriptor descriptor;
        ZLinkObjectReservation reservation;

        Fixture() {
            owner =
                    assertInstanceOf(
                                    ZLinkOwnerLeaseClaimed.class,
                                    await(repository.claimOwnerLease("owner", Duration.ofHours(1))))
                            .token();
            descriptor =
                    new ZLinkMeshNodeDescriptor(
                            "game",
                            RoutingId.from("target"),
                            Long.MIN_VALUE,
                            1,
                            "tcp://127.0.0.1:7100",
                            Map.of(),
                            1,
                            List.of(
                                    new ZLinkObjectCapability(
                                            ZLinkPlacementObjectKind.ACTOR,
                                            "actor",
                                            ZLinkObjectMaintenancePolicyKind.DISABLED,
                                            false,
                                            0)),
                            ZLinkMeshNodeObjectRole.SERVER,
                            Optional.of("entry"),
                            100,
                            new ZLinkPlacementCapacity(
                                    new ZLinkCapacityUsage(0, 0, 1),
                                    new ZLinkCapacityUsage(0, 0, 1),
                                    List.of()),
                            new ZLinkActivationConcurrency(0, 64),
                            Optional.empty(),
                            ZLinkFrameworkRuntimeState.SERVING,
                            "security",
                            owner.ownerId(),
                            owner.leaseGeneration(),
                            clock.instant());
            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    await(repository.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM))
                            .status());
            reservation =
                    assertInstanceOf(
                                    ZLinkObjectReserved.class,
                                    await(repository.reserve(request("actor"), () -> false)))
                            .reservation();
            provider.writes.clear();
        }

        ZLinkObjectReservationRequest request(String id) {
            return new ZLinkObjectReservationRequest(
                    ZLinkPlacementObjectKind.ACTOR,
                    ZLinkAuthorityKeyCodec.actor(id),
                    "actor",
                    "inline-v1:test",
                    new byte[32],
                    4,
                    new ZLinkMeshNodeDescriptorKey(descriptor.meshName(), descriptor.rid()),
                    descriptor.lifecycleGeneration(),
                    owner,
                    new byte[] {1},
                    ZLinkPlacementCapacityBundle.actor(1));
        }

        ZLinkCreationOperationTerminal terminal(ZLinkCreationTerminalState state) {
            byte[] envelope = new byte[] {0, 1, (byte) 0xff, (byte) state.ordinal()};
            return new ZLinkCreationOperationTerminal(
                    OPERATION,
                    reservation,
                    state,
                    envelope,
                    clock.instant().plus(ORIGINAL_DEADLINE).plus(RETENTION));
        }

        Object complete(ZLinkCreationOperationTerminal terminal) {
            return switch (terminal.state()) {
                case CREATED ->
                        await(
                                repository.commit(
                                        reservation, new byte[] {9}, terminal, () -> false));
                case REJECTED -> await(repository.reject(reservation, terminal, () -> false));
                case FAILED -> await(repository.abort(reservation, terminal, () -> false));
            };
        }
    }

    private static final class RecordingProvider implements ZLinkLocationStore {
        final StoreClock clock;
        final ZLinkInMemoryProviderLocationStore delegate;
        final List<ZLinkStoreWriteRequest> writes = new ArrayList<>();
        boolean conflictNextWrite;

        RecordingProvider(StoreClock clock) {
            this.clock = clock;
            delegate = new ZLinkInMemoryProviderLocationStore(clock);
        }

        List<ZLinkStorePut> terminalPuts() {
            return writes.getFirst().mutations().stream()
                    .filter(ZLinkStorePut.class::isInstance)
                    .map(ZLinkStorePut.class::cast)
                    .filter(put -> put.retention() != null)
                    .toList();
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return delegate.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            for (var mutation : request.mutations()) {
                if (mutation instanceof ZLinkStorePut put && put.bytes().length > 1024 * 1024) {
                    throw new IllegalArgumentException("provider value exceeds 1 MiB");
                }
            }
            writes.add(request);
            if (conflictNextWrite) {
                conflictNextWrite = false;
                return CompletableFuture.completedFuture(
                        new ZLinkStoreWriteConflict(clock.instant()));
            }
            return delegate.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }
    }

    private static final class StoreClock extends Clock {
        private final Instant origin = Instant.parse("2040-01-01T00:00:00Z");
        private long elapsedNanos;

        void advance(Duration elapsed) {
            if (elapsed.isNegative()) {
                throw new IllegalArgumentException("elapsed must be non-negative");
            }
            elapsedNanos = Math.addExact(elapsedNanos, elapsed.toNanos());
        }

        @Override
        public ZoneId getZone() {
            return ZoneOffset.UTC;
        }

        @Override
        public Clock withZone(ZoneId zone) {
            return Clock.fixed(instant(), zone);
        }

        @Override
        public Instant instant() {
            return origin.plusNanos(elapsedNanos);
        }
    }
}
