package systems.zlink.framework.locations.etcd;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.google.protobuf.ByteString;

import org.junit.jupiter.api.Test;

import java.nio.file.Path;
import java.util.HexFormat;

class ZLinkEtcdGoldenTest {
    static JsonNode vectors() throws Exception {
        return new ObjectMapper()
                .readTree(
                        Path.of("../../../runtime/protocol/golden/location-store-etcd-v1.json")
                                .toFile());
    }

    static ByteString hex(JsonNode node) {
        return ByteString.copyFrom(HexFormat.of().parseHex(node.asText()));
    }

    @Test
    void allKeysClocksMetadataAndVersionsMatchGolden() throws Exception {
        var root = vectors();
        for (var value : root.get("keyVectors")) {
            var ns = value.get("namespace").asText();
            var key = hex(value.get("frameworkKeyHex"));
            assertEquals(
                    hex(value.get("metadataKeyHex")),
                    EtcdEncoding.key(ns, 'm', key),
                    value.get("name").asText());
            assertEquals(hex(value.get("payloadKeyHex")), EtcdEncoding.key(ns, 'p', key));
            assertEquals(
                    hex(value.get("clockKeyHex")), EtcdEncoding.key(ns, 'c', ByteString.EMPTY));
        }
        for (var value : root.get("clockVectors")) {
            long time = Long.parseLong(value.get("value").asText());
            assertEquals(hex(value.get("hex")), EtcdEncoding.clock(time));
            assertEquals(time, EtcdEncoding.decodeClock(hex(value.get("hex"))));
        }
        for (var value : root.get("metadataVectors")) {
            var bytes = hex(value.get("metadataHex"));
            boolean expiry = value.get("hasExpiry").asBoolean();
            long time = Long.parseLong(value.get("expiresAtMs").asText());
            int length = value.get("payloadLength").asInt();
            assertEquals(bytes, EtcdEncoding.metadata(expiry, time, length));
            var decoded = EtcdEncoding.decodeMetadata(bytes);
            assertEquals(expiry, decoded.hasExpiry());
            assertEquals(time, decoded.expiresAt());
            assertEquals(length, decoded.payloadLength());
        }
        for (var value : root.get("storeVersions").get("accepted")) {
            long cluster = Long.parseUnsignedLong(value.get("clusterIdDecimal").asText());
            long revision = Long.parseLong(value.get("modRevision").asText());
            String encoded = value.get("value").asText();
            assertEquals(encoded, EtcdEncoding.version(cluster, revision));
            var decoded = EtcdEncoding.decodeVersion(encoded);
            assertNotNull(decoded);
            assertEquals(cluster, decoded.cluster());
            assertEquals(revision, decoded.revision());
        }
        for (var value : root.get("storeVersions").get("rejected"))
            assertNull(EtcdEncoding.decodeVersion(value.asText()));
    }

    @Test
    void everyCorruptGoldenRecordFailsThroughPublicRead() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            for (var value : vectors().get("corruptRecords")) {
                synchronized (backend) {
                    backend.records.clear();
                    backend.put(
                            EtcdTestServer.bytes("test\0c"),
                            value.has("clockHex")
                                    ? hex(value.get("clockHex"))
                                    : EtcdTestServer.clockBytes(1000));
                    if (value.hasNonNull("metadataHex"))
                        backend.put(
                                EtcdTestServer.bytes("test\0m\0k"), hex(value.get("metadataHex")));
                    if (value.hasNonNull("payloadHex"))
                        backend.put(
                                EtcdTestServer.bytes("test\0p\0k"), hex(value.get("payloadHex")));
                    if (value.hasNonNull("metadataHex")
                            && value.hasNonNull("payloadHex")
                            && !value.has("metadataModRevision")) {
                        var metadataKey = EtcdTestServer.bytes("test\0m\0k");
                        var payloadKey = EtcdTestServer.bytes("test\0p\0k");
                        backend.records.put(
                                metadataKey,
                                backend.records.get(metadataKey).toBuilder()
                                        .setModRevision(
                                                backend.records.get(payloadKey).getModRevision())
                                        .build());
                    }
                }
                var failure =
                        assertThrows(
                                Exception.class,
                                () ->
                                        ZLinkEtcdProviderTest.await(
                                                store.read(
                                                        new systems.zlink.framework.locationprovider
                                                                .ZLinkStoreKey("k"),
                                                        () -> false)),
                                value.get("name").asText());
                Throwable cause = failure;
                while (cause.getCause() != null) cause = cause.getCause();
                assertInstanceOf(java.io.IOException.class, cause, value.get("name").asText());
            }
        }
    }
}
