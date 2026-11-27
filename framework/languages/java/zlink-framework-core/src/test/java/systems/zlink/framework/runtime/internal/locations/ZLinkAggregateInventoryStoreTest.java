package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.MethodSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
import java.util.UUID;
import java.util.stream.Stream;

final class ZLinkAggregateInventoryStoreTest {
    private static final HexFormat HEX = HexFormat.of();
    private static final JsonNode GOLDEN = golden();

    private static JsonNode golden() {
        Path directory = Path.of("").toAbsolutePath();
        while (directory != null) {
            Path path =
                    directory.resolve(
                            "framework/runtime/protocol/golden/aggregate-storage-v1.json");
            if (Files.isRegularFile(path)) {
                try {
                    return new ObjectMapper().readTree(path.toFile());
                } catch (java.io.IOException failure) {
                    throw new IllegalStateException(
                            "cannot read aggregate storage golden", failure);
                }
            }
            directory = directory.getParent();
        }
        throw new IllegalStateException("aggregate storage golden is missing");
    }

    static Stream<String> acceptedCases() {
        return nodes(GOLDEN.get("cases")).map(vector -> vector.get("name").asText());
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
        assertEquals(
                request.participants().size(),
                store.load(fence, request.participants().size(), digest, () -> false)
                        .toCompletableFuture()
                        .join()
                        .size());
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

    private static Stream<JsonNode> nodes(JsonNode array) {
        return java.util.stream.StreamSupport.stream(array.spliterator(), false);
    }

    private static JsonNode named(String group, String name) {
        return nodes(GOLDEN.get(group))
                .filter(vector -> vector.get("name").asText().equals(name))
                .findFirst()
                .orElseThrow();
    }
}
