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
import org.junit.jupiter.params.provider.MethodSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletionException;
import java.util.stream.Stream;

final class ZLinkAggregateInventoryStoreTest {
    private static final HexFormat HEX = HexFormat.of();
    private static final JsonNode GOLDEN = golden();

    private static JsonNode golden() {
        String override = System.getenv("ZLINK_INVENTORY_STORAGE_GOLDEN");
        if (override != null) {
            return readGolden(Path.of(override));
        }
        Path directory = Path.of("").toAbsolutePath();
        while (directory != null) {
            Path path =
                    directory.resolve(
                            "framework/runtime/protocol/golden/aggregate-storage-v1.json");
            if (Files.isRegularFile(path)) {
                return readGolden(path);
            }
            directory = directory.getParent();
        }
        throw new IllegalStateException("aggregate storage golden is missing");
    }

    private static JsonNode readGolden(Path path) {
        try {
            return new ObjectMapper().readTree(path.toFile());
        } catch (java.io.IOException failure) {
            throw new IllegalStateException("cannot read aggregate storage golden", failure);
        }
    }

    static Stream<String> acceptedCases() {
        return nodes(GOLDEN.get("cases")).map(vector -> vector.get("name").asText());
    }

    static Stream<String> rejectedCases() {
        return nodes(GOLDEN.get("rejected")).map(vector -> vector.get("name").asText());
    }

    private static Stream<JsonNode> nodes(JsonNode array) {
        return java.util.stream.StreamSupport.stream(array.spliterator(), false);
    }

    private static JsonNode named(String group, String name) {
        return nodes(GOLDEN.get(group))
                .filter(vector -> vector.get("name").asText().equals(name))
                .findFirst()
                .orElseThrow();
    }

    @ParameterizedTest(name = "{0}")
    @MethodSource("acceptedCases")
    void writesCanonicalGoldenInventory(String name) {
        JsonNode vector = named("cases", name);
        var request = goldenRequest(vector);
        var fence = new ZLinkAggregateFence(request.aggregateId(), request.aggregateGeneration());
        var provider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkAggregateInventoryStore(provider);
        byte[] digest =
                store.store(fence, request.participants(), () -> false)
                        .toCompletableFuture()
                        .join();
        assertArrayEquals(request.inventoryDigest(), digest);
        assertEquals(
                providerKey(vector.get("aggregate").get("key").asText()),
                ZLinkProviderAuthorityRepository.aggregateKey(fence));
        assertArrayEquals(
                HEX.parseHex(vector.get("aggregate").get("jcsHex").asText()),
                ZLinkProviderAuthorityRepository.encodeAggregate(
                        ZLinkProviderAuthorityRepository.AGGREGATE_PREPARED, request));
        for (var value :
                Stream.concat(Stream.of(vector.get("root")), nodes(vector.get("pages"))).toList()) {
            assertArrayEquals(
                    HEX.parseHex(value.get("jcsHex").asText()),
                    read(provider, providerKey(value.get("key").asText())));
        }
        for (int index = 0; index < request.participants().size(); index++) {
            var participant = request.participants().get(index);
            String prefix =
                    vector.get("root")
                            .get("key")
                            .asText()
                            .replaceFirst("root$", "participant:" + index + ":");
            assertArrayEquals(
                    participant.authorityPayload(),
                    read(provider, providerKey(prefix + "authorityPayload")));
            assertArrayEquals(
                    participant.membershipMutation(),
                    read(provider, providerKey(prefix + "membershipMutation")));
        }
        JsonNode aggregate = vector.get("aggregate");
        provider.write(
                        new ZLinkStoreWriteRequest(
                                List.of(),
                                List.of(
                                        new ZLinkStorePut(
                                                providerKey(aggregate.get("key").asText()),
                                                HEX.parseHex(aggregate.get("jcsHex").asText()),
                                                null))),
                        () -> false)
                .toCompletableFuture()
                .join();
        var prepared =
                assertInstanceOf(
                        ZLinkAggregateAlreadyPrepared.class,
                        new ZLinkProviderLocationRepository(provider)
                                .prepareAggregate(request, () -> false)
                                .toCompletableFuture()
                                .join());
        assertEquals(fence, prepared.fence());
    }

    @Test
    void writesDirectJoinInventoryWithRelocationIdAndGenerationOne() {
        JsonNode vector = GOLDEN.get("directJoin");
        JsonNode entry = vector.get("pages").get(0).get("decoded").get("entries").get(0);
        JsonNode hashInputs = vector.get("participantHashInputs");
        var participant =
                new ZLinkAggregateParticipant(
                        entry.get("authorityKey").asText(),
                        1,
                        1,
                        entry.get("expectedStoreVersion").asText(),
                        ZLinkAuthorityGenerationTransition.PRESERVE,
                        HEX.parseHex(hashInputs.get("authorityPayloadHex").asText()),
                        HEX.parseHex(hashInputs.get("membershipMutationHex").asText()));
        var fence =
                ZLinkAggregateFence.forRelocation(
                        UUID.fromString(vector.get("relocationId").asText()));
        var provider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkAggregateInventoryStore(provider);
        byte[] digest =
                store.store(fence, List.of(participant), () -> false).toCompletableFuture().join();

        JsonNode root = vector.get("root");
        assertArrayEquals(HEX.parseHex(root.get("decoded").get("digestSha256").asText()), digest);
        assertArrayEquals(
                HEX.parseHex(root.get("jcsHex").asText()),
                read(provider, providerKey(root.get("key").asText())));
        for (JsonNode page : vector.get("pages")) {
            assertArrayEquals(
                    HEX.parseHex(page.get("jcsHex").asText()),
                    read(provider, providerKey(page.get("key").asText())));
        }
        JsonNode participantKeys = vector.get("participantKeys").get(0);
        assertArrayEquals(
                HEX.parseHex(hashInputs.get("authorityPayloadHex").asText()),
                read(provider, providerKey(participantKeys.get("authorityPayload").asText())));
        assertArrayEquals(
                HEX.parseHex(hashInputs.get("membershipMutationHex").asText()),
                read(provider, providerKey(participantKeys.get("membershipMutation").asText())));
    }

    @ParameterizedTest(name = "{0}")
    @MethodSource("rejectedCases")
    void rejectsDamagedGoldenInventory(String name) {
        JsonNode rejection = named("rejected", name);
        var base = named("cases", rejection.get("base").asText());
        var request = goldenRequest(base);
        var fence = new ZLinkAggregateFence(request.aggregateId(), request.aggregateGeneration());
        var provider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkAggregateInventoryStore(provider);
        byte[] digest =
                store.store(fence, request.participants(), () -> false)
                        .toCompletableFuture()
                        .join();
        for (var key : rejection.path("removeKeys")) {
            provider.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(new ZLinkStoreDelete(providerKey(key.asText())))),
                            () -> false)
                    .toCompletableFuture()
                    .join();
        }
        for (var value : rejection.path("replaceRecords")) {
            provider.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(
                                            new ZLinkStorePut(
                                                    providerKey(value.get("key").asText()),
                                                    HEX.parseHex(value.get("hex").asText()),
                                                    null))),
                            () -> false)
                    .toCompletableFuture()
                    .join();
        }
        byte[] expected =
                rejection.has("rootDigestSha256")
                        ? HEX.parseHex(rejection.get("rootDigestSha256").asText())
                        : digest;
        var failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                store.load(
                                                fence,
                                                request.participants().size(),
                                                expected,
                                                () -> false)
                                        .toCompletableFuture()
                                        .join());
        assertInstanceOf(
                ZLinkAggregateRelocationCoordinator.RelocationDataLostException.class,
                failure.getCause());
    }

    @ParameterizedTest
    @ValueSource(ints = {0, 1_048_577})
    void rejectsMembershipBodyOutsideSizeLimit(int length) {
        var store = new ZLinkAggregateInventoryStore(new ZLinkInMemoryProviderLocationStore());
        assertThrows(
                IllegalArgumentException.class,
                () ->
                        store.store(
                                new ZLinkAggregateFence(UUID.randomUUID(), 1),
                                List.of(participantWithMembership(length)),
                                () -> false));
    }

    @ParameterizedTest
    @ValueSource(ints = {1, 1_048_576})
    void acceptsMembershipBodyAtSizeLimit(int length) {
        var store = new ZLinkAggregateInventoryStore(new ZLinkInMemoryProviderLocationStore());
        var fence = new ZLinkAggregateFence(UUID.randomUUID(), 1);
        var participant = participantWithMembership(length);
        byte[] digest =
                store.store(fence, List.of(participant), () -> false).toCompletableFuture().join();
        var loaded = store.load(fence, 1, digest, () -> false).toCompletableFuture().join();
        assertArrayEquals(participant.membershipMutation(), loaded.getFirst().membershipMutation());
    }

    private static ZLinkAggregateParticipant participantWithMembership(int length) {
        byte[] membership = new byte[length];
        java.util.Arrays.fill(membership, (byte) 0xa5);
        return new ZLinkAggregateParticipant(
                "zla1:a:1:a",
                1,
                1,
                "version",
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                new byte[0],
                membership);
    }

    private static ZLinkAggregatePrepareRequest goldenRequest(JsonNode vector) {
        var aggregate = vector.get("aggregate").get("decoded");
        var inputs = vector.get("participantHashInputs");
        List<ZLinkAggregateParticipant> participants = new ArrayList<>();
        for (var page : vector.get("pages")) {
            for (var entry : page.get("decoded").get("entries")) {
                participants.add(
                        new ZLinkAggregateParticipant(
                                entry.get("authorityKey").asText(),
                                1,
                                1,
                                entry.get("expectedStoreVersion").asText(),
                                entry.get("ownerTransition").asText().equals("preserve")
                                        ? ZLinkAuthorityGenerationTransition.PRESERVE
                                        : ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                HEX.parseHex(inputs.get("authorityPayloadHex").asText()),
                                HEX.parseHex(inputs.get("membershipMutationHex").asText())));
            }
        }
        var descriptor = aggregate.get("targetDescriptor");
        return new ZLinkAggregatePrepareRequest(
                UUID.fromString(aggregate.get("aggregateId").asText()),
                Long.parseLong(aggregate.get("aggregateGeneration").asText()),
                participants,
                HEX.parseHex(aggregate.get("inventoryDigestSha256").asText()),
                new ZLinkMeshNodeDescriptorKey(
                        descriptor.get("meshName").asText(),
                        RoutingId.fromHex(descriptor.get("routingIdHex").asText())),
                Long.parseUnsignedLong(
                        aggregate.get("targetDescriptorLifecycleGeneration").asText()),
                ZLinkPlacementCapacityBundle.actor(participants.size()),
                new ZLinkLocationOwnerToken(
                        aggregate.get("targetOwner").get("ownerId").asText(),
                        Long.parseLong(
                                aggregate
                                        .get("targetOwner")
                                        .get("ownerLeaseGeneration")
                                        .asText())));
    }

    private static ZLinkStoreKey providerKey(String goldenKey) {
        return new ZLinkStoreKey(goldenKey.replaceFirst("^golden:", "zlink:v11:"));
    }

    private static byte[] read(ZLinkInMemoryProviderLocationStore provider, ZLinkStoreKey key) {
        return assertInstanceOf(
                        ZLinkStoreReadFound.class,
                        provider.read(key, () -> false).toCompletableFuture().join())
                .value()
                .bytes();
    }

    @Test
    void highBitDescriptorLifecycleTokenIsAcceptedByStoreRequests() {
        long highBit = Long.MIN_VALUE;
        var descriptor = new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-b"));
        var owner = new ZLinkLocationOwnerToken("owner-b", 1);
        assertEquals(
                highBit,
                new ZLinkObjectReservation(
                                "authority",
                                "store",
                                1,
                                1,
                                "reservation",
                                descriptor,
                                highBit,
                                owner)
                        .targetDescriptorLifecycleGeneration());
        assertEquals(
                highBit,
                new ZLinkAggregatePrepareRequest(
                                UUID.randomUUID(),
                                1,
                                List.of(
                                        new ZLinkAggregateParticipant(
                                                "authority",
                                                1,
                                                1,
                                                "store",
                                                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                                new byte[0],
                                                new byte[0])),
                                new byte[32],
                                descriptor,
                                highBit,
                                ZLinkPlacementCapacityBundle.actor(1),
                                owner)
                        .targetDescriptorLifecycleGeneration());
        assertEquals(
                highBit,
                new ZLinkAggregateRelocationCoordinator.Request(
                                UUID.randomUUID(),
                                1,
                                2,
                                List.of(
                                        new ZLinkAggregateRelocationCoordinator.Participant(
                                                "authority",
                                                systems.zlink.framework.locations
                                                        .ZLinkPlacementObjectKind.ACTOR,
                                                1,
                                                1,
                                                "store",
                                                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                                new byte[0],
                                                new byte[0])),
                                new byte[] {1},
                                descriptor,
                                highBit,
                                ZLinkPlacementCapacityBundle.actor(1),
                                owner,
                                "store")
                        .targetDescriptorLifecycleGeneration());
    }

    @Test
    void storesAndReadsInventoryAcrossTheLeafPageBound() {
        List<ZLinkAggregateParticipant> participants = new ArrayList<>();
        for (int index = 0; index < 2_050; index++) {
            participants.add(
                    new ZLinkAggregateParticipant(
                            "zla1:a:9:test-%04d".formatted(index),
                            index + 1L,
                            index + 2L,
                            "version-" + index,
                            ZLinkAuthorityGenerationTransition.NEW_OWNER,
                            new byte[] {(byte) index, 1, 2},
                            new byte[] {(byte) index, 3}));
        }
        var request =
                new ZLinkAggregatePrepareRequest(
                        UUID.randomUUID(),
                        1,
                        participants,
                        new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-b")),
                        1,
                        ZLinkPlacementCapacityBundle.actor(2_050),
                        new ZLinkLocationOwnerToken("owner-b", 1));
        var fence = new ZLinkAggregateFence(request.aggregateId(), request.aggregateGeneration());
        var store = new ZLinkAggregateInventoryStore(new ZLinkInMemoryProviderLocationStore());

        store.store(request, () -> false).toCompletableFuture().join();
        var loaded =
                store.load(
                                fence,
                                participants.size(),
                                ZLinkAggregateInventoryStore.digest(participants),
                                () -> false)
                        .toCompletableFuture()
                        .join();

        assertEquals(participants.size(), loaded.size());
        for (int index = 0; index < participants.size(); index++) {
            assertEquals(participants.get(index).authorityKey(), loaded.get(index).authorityKey());
            assertEquals(
                    participants.get(index).expectedStoreVersion(),
                    loaded.get(index).expectedStoreVersion());
            assertArrayEquals(
                    participants.get(index).authorityPayload(),
                    loaded.get(index).authorityPayload());
            assertArrayEquals(
                    participants.get(index).membershipMutation(),
                    loaded.get(index).membershipMutation());
        }
    }

    @Test
    void deletesAllInventoryValuesAcrossMultipleProviderPages() {
        List<ZLinkAggregateParticipant> participants = new ArrayList<>();
        for (int index = 0; index < 1_001; index++) {
            participants.add(
                    new ZLinkAggregateParticipant(
                            "zla1:a:9:test-%04d".formatted(index),
                            index + 1L,
                            index + 2L,
                            "version-" + index,
                            ZLinkAuthorityGenerationTransition.NEW_OWNER,
                            new byte[] {(byte) index, 1},
                            new byte[] {(byte) index, 2}));
        }
        var request =
                new ZLinkAggregatePrepareRequest(
                        UUID.randomUUID(),
                        2,
                        participants,
                        new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-b")),
                        1,
                        ZLinkPlacementCapacityBundle.actor(1_001),
                        new ZLinkLocationOwnerToken("owner-b", 1));
        var fence = new ZLinkAggregateFence(request.aggregateId(), request.aggregateGeneration());
        var provider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkAggregateInventoryStore(provider);

        store.store(request, () -> false).toCompletableFuture().join();
        store.delete(fence, () -> false).toCompletableFuture().join();
        store.delete(fence, () -> false).toCompletableFuture().join();

        var remaining =
                (ZLinkStoreScanPageResult)
                        provider.scan(
                                        new ZLinkStoreScanRequest(
                                                "zlink:v11:aggregate-inventory:"
                                                        + fence.aggregateId()
                                                        + ":"
                                                        + fence.aggregateGeneration()
                                                        + ":",
                                                null,
                                                1_000),
                                        () -> false)
                                .toCompletableFuture()
                                .join();
        assertTrue(remaining.value().items().isEmpty());
    }
}
