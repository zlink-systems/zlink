package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.condition.EnabledIfEnvironmentVariable;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.testsupport.ZLinkTestRedis;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletionException;

@EnabledIfEnvironmentVariable(named = "ZLINK_INVENTORY_MATRIX_MODE", matches = ".+")
final class ZLinkRedisAggregateInventoryMatrixTest {
    @ParameterizedTest(name = "{0}")
    @ValueSource(
            strings = {
                "small-3",
                "two-level-1025",
                "direct-join-1",
                "missing-page",
                "missing-participant",
                "damaged-participant",
                "damaged-page",
                "reordered-leaf-entries",
                "reordered-page-index"
            })
    @EnabledIfEnvironmentVariable(named = "ZLINK_INVENTORY_MATRIX_MODE", matches = "write|read")
    void sharedInventory(String name) throws IOException {
        String mode = System.getenv("ZLINK_INVENTORY_MATRIX_MODE");
        assertTrue(List.of("write", "read").contains(mode));
        String prefix = System.getenv("ZLINK_INVENTORY_MATRIX_PREFIX");
        assertTrue(prefix != null && !prefix.isBlank());
        JsonNode golden =
                new ObjectMapper()
                        .readTree(new File(System.getenv("ZLINK_INVENTORY_MATRIX_GOLDEN")));
        JsonNode rejection = named(golden.get("rejected"), name);
        String base = rejection == null ? name : rejection.get("base").asText();
        boolean direct = base.equals("direct-join-1");
        JsonNode vector = direct ? golden.get("directJoin") : named(golden.get("cases"), base);
        var aggregate = vector.path("aggregate").path("decoded");
        var fence =
                direct
                        ? ZLinkAggregateFence.forRelocation(
                                UUID.fromString(vector.get("relocationId").asText()))
                        : new ZLinkAggregateFence(
                                UUID.fromString(aggregate.get("aggregateId").asText()),
                                Long.parseUnsignedLong(
                                        aggregate.get("aggregateGeneration").asText()));
        byte[] digest = hex(vector.get("root").get("decoded"), "digestSha256");
        var participants = participants(vector);
        try (var provider =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(ZLinkTestRedis.endpoint())
                                .setKeyPrefix(prefix + ":" + name))) {
            var store = new ZLinkAggregateInventoryStore(provider);
            if (mode.equals("write")) {
                assertArrayEquals(
                        digest,
                        store.store(fence, participants, () -> false).toCompletableFuture().join());
                assertRecord(provider, vector.get("root"));
                for (var page : vector.get("pages")) {
                    assertRecord(provider, page);
                }
            } else if (rejection != null) {
                byte[] expected =
                        rejection.has("rootDigestSha256")
                                ? hex(rejection, "rootDigestSha256")
                                : digest;
                var failure =
                        assertThrows(
                                CompletionException.class,
                                () ->
                                        store.load(
                                                        fence,
                                                        participants.size(),
                                                        expected,
                                                        () -> false)
                                                .toCompletableFuture()
                                                .join());
                assertInstanceOf(
                        ZLinkAggregateRelocationCoordinator.RelocationDataLostException.class,
                        failure.getCause());
            } else {
                var actual =
                        store.load(fence, participants.size(), digest, () -> false)
                                .toCompletableFuture()
                                .join();
                assertEquals(participants.size(), actual.size());
                for (int i = 0; i < actual.size(); i++) {
                    var expected = participants.get(i);
                    var item = actual.get(i);
                    assertEquals(expected.authorityKey(), item.authorityKey());
                    assertEquals(expected.expectedStoreVersion(), item.expectedStoreVersion());
                    assertEquals(expected.ownerTransition(), item.ownerTransition());
                    assertArrayEquals(expected.authorityPayload(), item.authorityPayload());
                    assertArrayEquals(expected.membershipMutation(), item.membershipMutation());
                }
            }
        }
    }

    @ParameterizedTest(name = "SpotWide stale member: {0}")
    @ValueSource(booleans = {false, true})
    @EnabledIfEnvironmentVariable(named = "ZLINK_INVENTORY_MATRIX_MODE", matches = "spotwide-read")
    void spotWideSourceInventory(boolean staleMember) {
        String prefix = System.getenv("ZLINK_INVENTORY_MATRIX_PREFIX");
        try (var provider =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(ZLinkTestRedis.endpoint())
                                .setKeyPrefix(prefix + ":spotwide-" + staleMember))) {
            var coordinator =
                    new ZLinkAggregateRelocationCoordinator(
                            new ZLinkProviderLocationRepository(provider));
            var fence =
                    ZLinkAggregateFence.forRelocation(
                            UUID.fromString("00000000-0000-4000-8000-000000005354"));
            var restore =
                    coordinator
                            .readSealedInventory(
                                    fence, ZLinkAuthorityKeyCodec.spot("matrix-room"), () -> false)
                            .toCompletableFuture();
            if (staleMember) {
                var failure = assertThrows(CompletionException.class, restore::join);
                assertInstanceOf(
                        ZLinkAggregateRelocationCoordinator.RelocationDataLostException.class,
                        failure.getCause());
            } else {
                assertEquals(
                        List.of(
                                ZLinkAuthorityKeyCodec.actor("matrix-actor"),
                                ZLinkAuthorityKeyCodec.spot("matrix-room")),
                        restore.join().stream().map(entry -> entry.authority().key()).toList());
            }
        }
    }

    private static List<ZLinkAggregateParticipant> participants(JsonNode vector) {
        var inputs = vector.get("participantHashInputs");
        var payload = hex(inputs, "authorityPayloadHex");
        var mutation = hex(inputs, "membershipMutationHex");
        var participants = new ArrayList<ZLinkAggregateParticipant>();
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
                                payload,
                                mutation));
            }
        }
        return participants;
    }

    private static JsonNode named(JsonNode cases, String name) {
        for (var vector : cases) {
            if (vector.get("name").asText().equals(name)) {
                return vector;
            }
        }
        return null;
    }

    private static void assertRecord(ZLinkRedisLocationStore provider, JsonNode record) {
        String key = record.get("key").asText().replaceFirst("^golden:", "zlink:v11:");
        var found =
                assertInstanceOf(
                        ZLinkStoreReadFound.class,
                        provider.read(new ZLinkStoreKey(key), () -> false)
                                .toCompletableFuture()
                                .join());
        assertArrayEquals(hex(record, "jcsHex"), found.value().bytes(), key);
    }

    private static byte[] hex(JsonNode node, String field) {
        return HexFormat.of().parseHex(node.get(field).asText());
    }
}
