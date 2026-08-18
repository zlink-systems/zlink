package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import io.lettuce.core.RedisURI;
import io.lettuce.core.ScriptOutputType;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletionException;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreReadMissing;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;

/**
 * Drives the PRODUCTION Redis write/read path (not a from-scratch decoder)
 * against {@code golden/store-record-v1.json} (checklist C-4, item 5). This
 * complements {@code ZLinkStoreRecordGoldenTest} in zlink-framework-core,
 * which verifies the fixture bytes are internally self-consistent but never
 * touches a real store implementation.
 *
 * <p>Version and expiresAtMs are runtime-generated (a random version token,
 * a wall-clock expiry) so the full record cannot be byte-pinned against the
 * fixture's synthetic values the way the pure-function test can. What this
 * test pins instead, against a real redis-server, using the store's public
 * write/read API plus a raw byte-value probe of the stored ZSET member:</p>
 * <ul>
 *   <li>Redis key derivation ({@link ZLinkRedisLocationKeys#opaqueRecordKey})
 *       matches the golden {@code keyDerivation} vectors exactly.</li>
 *   <li>The stored member carries the {@code 0x01} format tag as a raw byte
 *       outside the cmsgpack payload.</li>
 *   <li>{@code rawBytes} (the caller's value bytes) round-trip byte-for-byte
 *       with no base64 sub-layer -- verified by writing the exact
 *       {@code jsonBytesHex} payload from the fixture and reading it back
 *       both through the opaque store's public read() and through a raw
 *       ZREVRANGE probe.</li>
 *   <li>Tombstones (delete) are encoded with {@code expiresAtMs == 0} and a
 *       genuine msgpack boolean tag ({@code 0xc3}), matching the
 *       {@code ownerLease-tombstone} vector's tail bytes.</li>
 *   <li>An unrecognized leading format tag is an explicit failure, not a
 *       silent miss (clean break, no read-old).</li>
 * </ul>
 */
final class ZLinkRedisStoreRecordGoldenConformanceTest {

    @Test
    void productionWritePathMatchesGoldenKeysAndWireShape() throws IOException {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");

        JsonNode fixture = readTree(sharedFixturePath());
        String prefix = fixture.path("prefixExample").asText();
        ZLinkRedisLocationKeys keys = new ZLinkRedisLocationKeys(prefix);

        for (JsonNode key : fixture.path("keyDerivation")) {
            String originalKey = key.path("preimagePrintable").asText()
                .replace("\\u0000", "\0");
            // The fixture's preimage for opaque records is
            // "<recordKind>\0<...>", identical to the store's original key.
            // Owner ruling (2026-08-19): the Redis Cluster hashtag braces
            // around the domain tag are canonical
            // ("{prefix}:{zlink-location-v3}:opaque:{sha256hex}"); the
            // on-disk golden's brace-less redisKey field predates that
            // ruling and is being corrected separately, so the expected key
            // here is rebuilt with braces rather than compared verbatim
            // against the stale fixture field. The sha256hex component is
            // still the fixture's normative byte truth.
            String expectedKey = prefix + ":{zlink-location-v3}:opaque:"
                + key.path("sha256Hex").asText();
            assertEquals(
                expectedKey,
                keys.opaqueRecordKey(originalKey),
                "redis key derivation mismatch: " + key.path("record").asText());
        }

        String storePrefix = "goldenconf:" + UUID.randomUUID();
        ZLinkRedisLocationKeys storeKeys = new ZLinkRedisLocationKeys(storePrefix);
        try (var store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions()
                .setConnectionString(endpoint)
                .setKeyPrefix(storePrefix))) {

            Iterator<JsonNode> vectors =
                fixture.path("valueVectors").path("genericOpaqueRecord").elements();
            while (vectors.hasNext()) {
                JsonNode vector = vectors.next();
                if (vector.path("tombstone").asBoolean()) {
                    continue;
                }
                String name = vector.path("name").asText();
                String originalKey = vector.path("originalKey").asText()
                    .replace("\\u0000", "\0");
                byte[] jsonBytes = HexFormat.of().parseHex(
                    vector.path("jsonBytesHex").asText());

                var key = new ZLinkStoreKey(originalKey);
                var applied = assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    store.write(
                        new ZLinkStoreWriteRequest(
                            List.of(),
                            List.of(new ZLinkStorePut(key, jsonBytes, null))),
                        () -> false).toCompletableFuture().join(),
                    "write did not apply: " + name);
                assertTrue(applied.putVersions().containsKey(key));

                var found = assertInstanceOf(
                    ZLinkStoreReadFound.class,
                    store.read(key, () -> false).toCompletableFuture().join(),
                    "record not readable back: " + name);
                assertArrayEquals(
                    jsonBytes,
                    found.value().bytes(),
                    "raw payload round-trip mismatch (no base64 layer): " + name);

                byte[] member = readRawZsetMember(
                    endpoint, storeKeys.opaqueRecordKey(originalKey));
                assertEquals((byte) 0x01, member[0], "format tag mismatch: " + name);
                OpaqueMember decoded = decodeOpaqueMember(member, 1, member.length);
                assertEquals(originalKey, decoded.originalKey,
                    "originalKey mismatch: " + name);
                assertArrayEquals(jsonBytes, decoded.rawBytes,
                    "cmsgpack str-family payload mismatch: " + name);
                assertFalse(decoded.tombstone, "unexpected tombstone: " + name);

                store.write(
                    new ZLinkStoreWriteRequest(
                        List.of(),
                        List.of(new ZLinkStoreDelete(key))),
                    () -> false).toCompletableFuture().join();

                byte[] tombstoneMember = readRawZsetMember(
                    endpoint, storeKeys.opaqueRecordKey(originalKey));
                OpaqueMember tombstone = decodeOpaqueMember(
                    tombstoneMember, 1, tombstoneMember.length);
                assertTrue(tombstone.tombstone,
                    "delete did not set the tombstone boolean: " + name);
                assertEquals(0L, tombstone.expiresAtMs,
                    "tombstone expiresAtMs sentinel must be 0: " + name);
                assertInstanceOf(
                    ZLinkStoreReadMissing.class,
                    store.read(key, () -> false).toCompletableFuture().join(),
                    "tombstoned record still reads as found: " + name);
            }
        }
    }

    /**
     * Drives {@code ZLinkProviderLocationRepository.updateMeshNode} -- the
     * actual production MeshNode descriptor writer, not a from-scratch
     * encoder -- against the full-field {@code meshNodeDescriptor-normal}
     * vector (21-location-runtime.md#2.4), then reads the raw stored bytes
     * back and structurally compares them to the fixture's {@code decoded}
     * JSON. {@code leaseGeneration} is the one field the fixture can't pin
     * ahead of time (it's issued by the live owner-lease claim, still on
     * its own pre-migration encoding until that record type's turn), so it
     * is substituted with the real claimed value on both sides before the
     * comparison.
     */
    @Test
    void productionMeshNodeDescriptorWriteMatchesFullFieldVector()
        throws Exception {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");

        JsonNode fixture = readTree(sharedFixturePath());
        JsonNode keyVector = keyDerivationVector(fixture, "mesh-node-descriptor");
        JsonNode expectedRecord = valueVector(fixture, "meshNodeDescriptor-normal")
            .path("decoded");
        JsonNode expectedDescriptor = expectedRecord.path("descriptor");

        String storePrefix = "goldenconf-mesh:" + UUID.randomUUID();
        try (var store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions()
                .setConnectionString(endpoint)
                .setKeyPrefix(storePrefix))) {
            var repository = new ZLinkProviderLocationRepository(store);
            var owner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                repository.claimOwnerLease(
                        expectedRecord.path("ownerId").asText(),
                        Duration.ofMinutes(5))
                    .toCompletableFuture().get());

            var descriptor = new ZLinkMeshNodeDescriptor(
                expectedDescriptor.path("meshName").asText(),
                RoutingId.fromHex(
                    expectedDescriptor.path("routingIdHex").asText()),
                Long.parseLong(
                    expectedDescriptor.path("lifecycleGeneration").asText()),
                Long.parseLong(
                    expectedDescriptor.path("descriptorRevision").asText()),
                expectedDescriptor.path("endpoint").asText(),
                toChannelWeights(expectedDescriptor.path("channelWeights")),
                Long.parseLong(
                    expectedDescriptor.path("applicationVersion").asText()),
                toCapabilities(
                    expectedDescriptor.path("objectCapabilities")),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.of(expectedDescriptor.path("entrySpotId").asText()),
                expectedDescriptor.path("placementWeight").asInt(),
                toCapacity(expectedDescriptor.path("capacity")),
                new ZLinkActivationConcurrency(
                    expectedDescriptor.path("activationConcurrency")
                        .path("active").asInt(),
                    expectedDescriptor.path("activationConcurrency")
                        .path("limit").asInt()),
                Optional.empty(),
                ZLinkFrameworkRuntimeState.SERVING,
                expectedDescriptor.path("securityIdentity").asText(),
                owner.token().ownerId(),
                owner.token().leaseGeneration(),
                Instant.ofEpochMilli(Long.parseLong(
                    expectedDescriptor.path("updatedAtEpochMs").asText())));

            var written = repository.updateMeshNode(
                    descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get();
            assertEquals(ZLinkLocationWriteStatus.STORED, written.status());

            String preimage = "mesh-node\0" + descriptor.meshName() + "\0"
                + descriptor.rid().toHex();
            assertEquals(
                keyVector.path("preimagePrintable").asText()
                    .replace("\\u0000", "\0"),
                preimage,
                "mesh-node preimage does not match the golden vector");

            var raw = assertInstanceOf(
                ZLinkStoreReadFound.class,
                store.read(new ZLinkStoreKey(preimage), () -> false)
                    .toCompletableFuture().get());
            JsonNode actual = new ObjectMapper().readTree(raw.value().bytes());

            String leaseGeneration =
                Long.toString(owner.token().leaseGeneration());
            ObjectNode expected = expectedRecord.deepCopy();
            expected.put("leaseGeneration", leaseGeneration);
            ((ObjectNode) expected.path("descriptor"))
                .put("leaseGeneration", leaseGeneration);

            assertEquals(
                expected,
                actual,
                "canonical JSON field mismatch against the golden vector");
        }
    }

    /**
     * Drives {@code ZLinkProviderLocationRepository.claimOwnerLease} -- the
     * real production owner-lease writer -- against the {@code
     * ownerLease-expired} vector (21-location-runtime.md#2.4), then reads
     * the raw stored bytes back and structurally compares them to the
     * fixture's {@code decoded} JSON. {@code leaseGeneration} is issued by
     * the live Store-wide sequence, so it's substituted with the real
     * claimed value on both sides before the comparison, mirroring the
     * MeshNode descriptor conformance test above.
     */
    @Test
    void productionOwnerLeaseClaimMatchesFullFieldVector() throws Exception {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");

        JsonNode fixture = readTree(sharedFixturePath());
        JsonNode keyVector = keyDerivationVector(fixture, "owner-lease");
        JsonNode expected = valueVector(fixture, "ownerLease-expired")
            .path("decoded").deepCopy();
        String ownerId = expected.path("ownerId").asText();

        String storePrefix = "goldenconf-owner:" + UUID.randomUUID();
        try (var store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions()
                .setConnectionString(endpoint)
                .setKeyPrefix(storePrefix))) {
            var repository = new ZLinkProviderLocationRepository(store);
            var claimed = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                repository.claimOwnerLease(ownerId, Duration.ofMinutes(5))
                    .toCompletableFuture().get());

            String preimage = "owner-lease\0" + ownerId;
            assertEquals(
                keyVector.path("preimagePrintable").asText()
                    .replace("\\u0000", "\0"),
                preimage,
                "owner-lease preimage does not match the golden vector");

            var raw = assertInstanceOf(
                ZLinkStoreReadFound.class,
                store.read(new ZLinkStoreKey(preimage), () -> false)
                    .toCompletableFuture().get());
            JsonNode actual = new ObjectMapper().readTree(raw.value().bytes());

            ((ObjectNode) expected).put(
                "leaseGeneration",
                Long.toString(claimed.token().leaseGeneration()));

            assertEquals(
                expected,
                actual,
                "canonical JSON field mismatch against the golden vector");
        }
    }

    @Test
    void unrecognizedFormatTagFailsExplicitlyInsteadOfSilentlyMissing()
        throws IOException {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");

        String prefix = "goldenconf-tag:" + UUID.randomUUID();
        ZLinkRedisLocationKeys keys = new ZLinkRedisLocationKeys(prefix);
        var key = new ZLinkStoreKey("mesh-node\0main\0" + "01020304");
        String redisKey = keys.opaqueRecordKey(key.value());

        var connection = ZLinkRedisLocationConnection.forBytes(
            RedisURI.create(endpoint));
        try {
            byte[] corrupted = new byte[] {0x02, (byte) 0x90};
            connection.commands()
                .thenCompose(commands -> commands.<Long>eval(
                    "return redis.call('ZADD', KEYS[1], 1, ARGV[1])",
                    ScriptOutputType.INTEGER,
                    new String[] {redisKey},
                    corrupted))
                .toCompletableFuture()
                .join();

            try (var store = new ZLinkRedisLocationStore(
                new ZLinkRedisLocationOptions()
                    .setConnectionString(endpoint)
                    .setKeyPrefix(prefix))) {
                CompletionException failure = assertThrows(
                    CompletionException.class,
                    () -> store.read(key, () -> false)
                        .toCompletableFuture()
                        .join());
                assertTrue(
                    failure.getCause() != null
                        && failure.getCause().getMessage() != null
                        && failure.getCause().getMessage()
                            .contains("zlink-opaque-record-tag"),
                    "expected an explicit tag-format failure, got: " + failure);
            }
        } finally {
            connection.closeAsync().toCompletableFuture().join();
        }
    }

    private static byte[] readRawZsetMember(String endpoint, String redisKey) {
        var connection = ZLinkRedisLocationConnection.<byte[]>forBytes(
            RedisURI.create(endpoint));
        try {
            List<Object> members = connection.commands()
                .thenCompose(commands -> commands.<List<Object>>eval(
                    "return redis.call('ZREVRANGE', KEYS[1], 0, 0)",
                    ScriptOutputType.MULTI,
                    new String[] {redisKey}))
                .toCompletableFuture()
                .join();
            return (byte[]) members.get(0);
        } finally {
            connection.closeAsync().toCompletableFuture().join();
        }
    }

    private record OpaqueMember(
        String originalKey, byte[] rawBytes, String version,
        long expiresAtMs, boolean tombstone) {
    }

    private static OpaqueMember decodeOpaqueMember(byte[] bytes, int offset, int end) {
        int[] cursor = {offset};
        int count = readArrayHead(bytes, cursor);
        if (count != 5) {
            throw new IllegalStateException("invalid opaque member arity: " + count);
        }
        String originalKey = new String(readStr(bytes, cursor), StandardCharsets.UTF_8);
        byte[] rawBytes = readStr(bytes, cursor);
        String version = new String(readStr(bytes, cursor), StandardCharsets.UTF_8);
        long expiresAtMs = readUint(bytes, cursor);
        boolean tombstone = readBool(bytes, cursor);
        if (cursor[0] != end) {
            throw new IllegalStateException("trailing byte after opaque member");
        }
        return new OpaqueMember(originalKey, rawBytes, version, expiresAtMs, tombstone);
    }

    private static int nextByte(byte[] bytes, int[] cursor) {
        return bytes[cursor[0]++] & 0xff;
    }

    private static int readArrayHead(byte[] bytes, int[] cursor) {
        int tag = nextByte(bytes, cursor);
        if ((tag & 0xf0) == 0x90) {
            return tag & 0x0f;
        }
        if (tag == 0xdc) {
            return (nextByte(bytes, cursor) << 8) | nextByte(bytes, cursor);
        }
        if (tag == 0xdd) {
            int v = 0;
            for (int i = 0; i < 4; i++) {
                v = (v << 8) | nextByte(bytes, cursor);
            }
            return v;
        }
        throw new IllegalStateException("invalid msgpack array tag: " + tag);
    }

    private static byte[] readStr(byte[] bytes, int[] cursor) {
        int tag = nextByte(bytes, cursor);
        int length;
        if ((tag & 0xe0) == 0xa0) {
            length = tag & 0x1f;
        } else if (tag == 0xd9) {
            length = nextByte(bytes, cursor);
        } else if (tag == 0xda) {
            length = (nextByte(bytes, cursor) << 8) | nextByte(bytes, cursor);
        } else if (tag == 0xdb) {
            length = 0;
            for (int i = 0; i < 4; i++) {
                length = (length << 8) | nextByte(bytes, cursor);
            }
        } else {
            throw new IllegalStateException("invalid msgpack str tag: " + tag);
        }
        byte[] value = java.util.Arrays.copyOfRange(bytes, cursor[0], cursor[0] + length);
        cursor[0] += length;
        return value;
    }

    private static long readUint(byte[] bytes, int[] cursor) {
        int tag = nextByte(bytes, cursor);
        if ((tag & 0x80) == 0) {
            return tag;
        }
        if (tag == 0xcc) {
            return nextByte(bytes, cursor);
        }
        if (tag == 0xcd) {
            return ((long) nextByte(bytes, cursor) << 8) | nextByte(bytes, cursor);
        }
        if (tag == 0xce) {
            long v = 0;
            for (int i = 0; i < 4; i++) {
                v = (v << 8) | nextByte(bytes, cursor);
            }
            return v;
        }
        if (tag == 0xcf) {
            long v = 0;
            for (int i = 0; i < 8; i++) {
                v = (v << 8) | nextByte(bytes, cursor);
            }
            return v;
        }
        throw new IllegalStateException("invalid msgpack uint tag: " + tag);
    }

    private static boolean readBool(byte[] bytes, int[] cursor) {
        int tag = nextByte(bytes, cursor);
        if (tag == 0xc2) {
            return false;
        }
        if (tag == 0xc3) {
            return true;
        }
        throw new IllegalStateException("invalid msgpack bool tag: " + tag);
    }

    private static JsonNode keyDerivationVector(JsonNode fixture, String record) {
        for (JsonNode key : fixture.path("keyDerivation")) {
            if (record.equals(key.path("record").asText())) {
                return key;
            }
        }
        throw new IllegalStateException(
            "no keyDerivation vector named: " + record);
    }

    private static JsonNode valueVector(JsonNode fixture, String name) {
        for (JsonNode vector : fixture.path("valueVectors")
                .path("genericOpaqueRecord")) {
            if (name.equals(vector.path("name").asText())) {
                return vector;
            }
        }
        throw new IllegalStateException("no value vector named: " + name);
    }

    private static Map<String, Integer> toChannelWeights(JsonNode node) {
        Map<String, Integer> weights = new LinkedHashMap<>();
        node.fields().forEachRemaining(
            entry -> weights.put(entry.getKey(), entry.getValue().asInt()));
        return weights;
    }

    private static List<ZLinkObjectCapability> toCapabilities(JsonNode node) {
        List<ZLinkObjectCapability> capabilities = new ArrayList<>();
        node.forEach(capability -> capabilities.add(new ZLinkObjectCapability(
            objectKindFromWire(capability.path("objectKind").asText()),
            capability.path("stableType").asText(),
            policyFromWire(capability.path("policy").asText()),
            capability.path("hasSnapshotAdapter").asBoolean(),
            capability.path("limit").asInt())));
        return capabilities;
    }

    private static ZLinkPlacementCapacity toCapacity(JsonNode node) {
        List<ZLinkSpotTypeCapacity> spotTypes = new ArrayList<>();
        node.path("spotTypes").forEach(spotType -> spotTypes.add(
            new ZLinkSpotTypeCapacity(
                objectKindFromWire(spotType.path("objectKind").asText()),
                spotType.path("stableType").asText(),
                toUsage(spotType))));
        return new ZLinkPlacementCapacity(
            toUsage(node.path("actors")),
            toUsage(node.path("spots")),
            spotTypes);
    }

    private static ZLinkCapacityUsage toUsage(JsonNode node) {
        return new ZLinkCapacityUsage(
            node.path("active").asInt(),
            node.path("reserved").asInt(),
            node.path("limit").asInt());
    }

    private static ZLinkPlacementObjectKind objectKindFromWire(String value) {
        return switch (value) {
            case "actor" -> ZLinkPlacementObjectKind.ACTOR;
            case "userSpot" -> ZLinkPlacementObjectKind.USER_SPOT;
            case "instanceSpot" -> ZLinkPlacementObjectKind.INSTANCE_SPOT;
            default -> throw new IllegalStateException(
                "unrecognized objectKind: " + value);
        };
    }

    private static ZLinkObjectMaintenancePolicyKind policyFromWire(
        String value) {
        return switch (value) {
            case "disabled" -> ZLinkObjectMaintenancePolicyKind.DISABLED;
            case "recreate" -> ZLinkObjectMaintenancePolicyKind.RECREATE;
            case "snapshot" -> ZLinkObjectMaintenancePolicyKind.SNAPSHOT;
            default -> throw new IllegalStateException(
                "unrecognized policy: " + value);
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

    private static JsonNode readTree(Path path) throws IOException {
        return new ObjectMapper().readTree(Files.readString(path));
    }
}
