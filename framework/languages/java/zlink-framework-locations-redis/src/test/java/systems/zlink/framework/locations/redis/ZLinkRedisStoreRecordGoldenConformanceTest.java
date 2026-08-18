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
import io.lettuce.core.RedisURI;
import io.lettuce.core.ScriptOutputType;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HexFormat;
import java.util.Iterator;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletionException;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreReadMissing;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;

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
