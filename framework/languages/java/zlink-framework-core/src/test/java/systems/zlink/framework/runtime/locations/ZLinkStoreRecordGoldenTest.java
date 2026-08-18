package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.io.UnsupportedEncodingException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.Iterator;
import org.junit.jupiter.api.Test;

/**
 * Target-contract pin for checklist C-3 (store record golden fixture:
 * 21-location-runtime.md#2.4, 22-location-store-redis.md#7). This test
 * consumes {@code golden/store-record-v1.json} directly, independent of any
 * production opaque-record store codec — no language has implemented the
 * {@code zlink-location-v3} opaque record write path yet (checklist C-4).
 * It stays green today because sha256 key derivation and cmsgpack value
 * decoding need nothing from C-4. The cmsgpack member decoder below is
 * written from scratch against the MessagePack type table in
 * 22-location-store-redis.md#7 (str family for strings/bytes, unsigned int
 * family for expiresAtMs, bool for tombstone) rather than reusing any
 * project msgpack dependency, so it is a genuinely independent check.
 */
final class ZLinkStoreRecordGoldenTest {
    @Test
    void keyDerivationAndValueVectorsDecodeAsPinned() throws IOException, NoSuchAlgorithmException {
        JsonNode fixture = new ObjectMapper().readTree(Files.readString(sharedFixturePath()));
        String prefix = fixture.path("prefixExample").asText();
        String namespaceTag = fixture.path("namespace").asText();

        for (JsonNode key : fixture.path("keyDerivation")) {
            byte[] preimage = HexFormat.of().parseHex(key.path("preimageHex").asText());
            String sha256Hex = sha256Hex(preimage);
            assertEquals(key.path("sha256Hex").asText(), sha256Hex,
                "sha256 mismatch: " + key.path("record").asText());
            assertEquals(
                prefix + ":" + namespaceTag + ":" + sha256Hex,
                key.path("redisKey").asText(),
                "redis key assembly mismatch: " + key.path("record").asText());
        }

        JsonNode relocationBlob = fixture.path("relocationBlob");
        byte[] relocationBytes = HexFormat.of().parseHex(relocationBlob.path("rawBytesHex").asText());
        assertTrue(relocationBytes.length > 0);
        assertEquals(
            prefix + ":{zlink-relocation-v1}:blob:" + relocationBlob.path("reference").asText(),
            relocationBlob.path("redisKey").asText());

        Iterator<JsonNode> vectors = fixture.path("valueVectors").path("genericOpaqueRecord").elements();
        while (vectors.hasNext()) {
            JsonNode vector = vectors.next();
            String name = vector.path("name").asText();
            byte[] full = HexFormat.of().parseHex(vector.path("fullValueHex").asText());
            assertEquals((byte) 0x01, full[0], "format tag mismatch: " + name);
            OpaqueMember decoded = decodeOpaqueMember(full, 1, full.length);

            String expectedOriginalKey = vector.path("originalKey").asText().replace("\\u0000", "\0");
            assertEquals(expectedOriginalKey, decoded.originalKey, "originalKey mismatch: " + name);
            assertEquals(vector.path("jsonBytesHex").asText(), HexFormat.of().formatHex(decoded.rawBytes),
                "rawBytes mismatch: " + name);
            assertEquals(vector.path("version").asText(), decoded.version, "version mismatch: " + name);
            assertEquals(vector.path("expiresAtMs").asText(), Long.toUnsignedString(decoded.expiresAtMs),
                "expiresAtMs mismatch: " + name);
            assertEquals(vector.path("tombstone").asBoolean(), decoded.tombstone, "tombstone mismatch: " + name);

            byte[] member = HexFormat.of().parseHex(vector.path("cmsgpackMemberHex").asText());
            assertArrayEquals(member, java.util.Arrays.copyOfRange(full, 1, full.length),
                "cmsgpack member/full byte mismatch: " + name);

            if (!vector.path("tombstone").asBoolean()) {
                JsonNode parsed = new ObjectMapper().readTree(decoded.rawBytes);
                assertEquals(vector.path("decoded"), parsed, "JSON field mismatch: " + name);
            } else {
                assertEquals(0, decoded.rawBytes.length, "tombstone must carry empty rawBytes: " + name);
            }
        }
    }

    private record OpaqueMember(String originalKey, byte[] rawBytes, String version, long expiresAtMs,
            boolean tombstone) {
    }

    private static OpaqueMember decodeOpaqueMember(byte[] bytes, int offset, int end)
        throws UnsupportedEncodingException {
        int[] cursor = {offset};
        int count = readArrayHead(bytes, cursor);
        if (count != 5) fail("invalid opaque member arity: " + count);
        String originalKey = new String(readStr(bytes, cursor), StandardCharsets.UTF_8);
        byte[] rawBytes = readStr(bytes, cursor);
        String version = new String(readStr(bytes, cursor), StandardCharsets.UTF_8);
        long expiresAtMs = readUint(bytes, cursor);
        boolean tombstone = readBool(bytes, cursor);
        if (cursor[0] != end) fail("trailing byte after opaque member");
        return new OpaqueMember(originalKey, rawBytes, version, expiresAtMs, tombstone);
    }

    private static int nextByte(byte[] bytes, int[] cursor) {
        return bytes[cursor[0]++] & 0xff;
    }

    private static int readArrayHead(byte[] bytes, int[] cursor) {
        int tag = nextByte(bytes, cursor);
        if ((tag & 0xf0) == 0x90) return tag & 0x0f;
        if (tag == 0xdc) {
            int v = ((nextByte(bytes, cursor) << 8) | nextByte(bytes, cursor));
            return v;
        }
        if (tag == 0xdd) {
            int v = 0;
            for (int i = 0; i < 4; i++) v = (v << 8) | nextByte(bytes, cursor);
            return v;
        }
        fail("invalid msgpack array tag: " + tag);
        return -1;
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
            for (int i = 0; i < 4; i++) length = (length << 8) | nextByte(bytes, cursor);
        } else {
            fail("invalid msgpack str tag: " + tag);
            return new byte[0];
        }
        byte[] value = java.util.Arrays.copyOfRange(bytes, cursor[0], cursor[0] + length);
        cursor[0] += length;
        return value;
    }

    private static long readUint(byte[] bytes, int[] cursor) {
        int tag = nextByte(bytes, cursor);
        if ((tag & 0x80) == 0) return tag;
        if (tag == 0xcc) return nextByte(bytes, cursor);
        if (tag == 0xcd) return ((long) nextByte(bytes, cursor) << 8) | nextByte(bytes, cursor);
        if (tag == 0xce) {
            long v = 0;
            for (int i = 0; i < 4; i++) v = (v << 8) | nextByte(bytes, cursor);
            return v;
        }
        if (tag == 0xcf) {
            long v = 0;
            for (int i = 0; i < 8; i++) v = (v << 8) | nextByte(bytes, cursor);
            return v;
        }
        fail("invalid msgpack uint tag: " + tag);
        return -1;
    }

    private static boolean readBool(byte[] bytes, int[] cursor) {
        int tag = nextByte(bytes, cursor);
        if (tag == 0xc2) return false;
        if (tag == 0xc3) return true;
        fail("invalid msgpack bool tag: " + tag);
        return false;
    }

    private static String sha256Hex(byte[] input) throws NoSuchAlgorithmException {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(input);
        return HexFormat.of().formatHex(digest);
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
}
