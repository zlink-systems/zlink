package systems.zlink.framework.locations.redis;

import systems.zlink.framework.runtime.internal.locations.ZLinkLocationDescriptorCodec;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;

class ZLinkRedisLocationRowJsonTest {
    private static final ObjectMapper JSON = new ObjectMapper();
    private static final Instant UPDATED_AT = Instant.parse("2026-07-03T00:00:00Z");

    @Test
    void fanoutPublisherRowRoundTripsDedicatedDescriptorFields() {
        ZLinkFanoutPublisherDescriptor original =
            new ZLinkFanoutPublisherDescriptor(
                "events",
                RoutingId.from(new byte[] {0x00, (byte) 0xff, 0x01}),
                7,
                11,
                "tcp://127.0.0.1:7400",
                ZLinkFrameworkRuntimeState.SERVING,
                "cluster-a",
                "owner-a",
                3,
                UPDATED_AT);

        ZLinkFanoutPublisherDescriptor decoded =
            ZLinkLocationDescriptorCodec.deserializeFanoutPublisher(
                ZLinkLocationDescriptorCodec.serializeFanoutPublisher(original),
                3,
                UPDATED_AT);

        assertEquals(original, decoded);
        assertEquals(
            ZLinkLocationDescriptorCodec
                .fanoutPublisherImmutableFingerprint(original),
            ZLinkLocationDescriptorCodec
                .fanoutPublisherImmutableFingerprint(decoded));
    }

    @Test
    void canonicalHybridSchemaUsesOneFixedProviderHashTag() {
        assertThrows(
            IllegalArgumentException.class,
            () -> new ZLinkRedisLocationOptions()
                .setKeyPrefix("bad:{caller-tag}"));
        var keys = new ZLinkRedisLocationKeys("app");
        assertEquals(
            "app:{zlink-location-v3}:schema",
            keys.schemaKey());
        assertEquals(
            "app:{zlink-location-v3}:counter",
            keys.counterKey());
        assertEquals(
            "app:{zlink-location-v3}:authority:key-index",
            keys.authorityIndexKey());
        assertEquals(
            "app:{zlink-location-v3}:membership:current",
            keys.authorityMembershipsKey());
        String current = keys.authorityRowKey(
            "zla1:a:4:game:7:actor-1");
        assertTrue(current.matches(
            "app:\\{zlink-location-v3}:authority:current:"
                + "[0-9a-f]{64}"));
        assertTrue(keys.leaseKey("owner-a").matches(
            "app:\\{zlink-location-v3}:owner-lease:"
                + "[0-9a-f]{64}"));
    }

    @Test
    void actorLocationV2FixturePinsCanonicalRedisShape() throws Exception {
        JsonNode root = JSON.readTree(Files.readString(fixturePath()));

        assertEquals(
            List.of(
                "payload",
                "storeVersion",
                "objectGeneration",
                "authorityOwnerGeneration",
                "ownerId",
                "ownerLeaseGeneration"),
            JSON.convertValue(
                root.path("hashFields"),
                JSON.getTypeFactory().constructCollectionType(List.class, String.class)));
        JsonNode row = root.path("row");
        assertEquals("actor", row.path("kind").asText());
        assertEquals("zla1:a:7:actor-1", row.path("key").asText());
        JsonNode hash = row.path("hash");
        assertEquals("opaque-actor-authority-v1", hash.path("payload").asText());
        assertEquals("101", hash.path("storeVersion").asText());
        assertEquals("11", hash.path("objectGeneration").asText());
        assertEquals("4", hash.path("authorityOwnerGeneration").asText());
        assertEquals("actor-owner-a", hash.path("ownerId").asText());
        assertEquals("9", hash.path("ownerLeaseGeneration").asText());
    }

    @Test
    void meshNodeDescriptorFixturePinsCanonicalJsonBytes()
        throws Exception {
        JsonNode root = JSON.readTree(Files.readString(
            descriptorFixturePath()));
        JsonNode hash = root.path("row").path("hash");
        ZLinkMeshNodeDescriptor descriptor =
            new ZLinkMeshNodeDescriptor(
                "game",
                RoutingId.fromHex("67616d652d61"),
                7,
                3,
                "tcp://10.0.0.1:7300",
                Map.of("orders", 100, "world", 50),
                0,
                List.of(),
                ZLinkMeshNodeObjectRole.NONE,
                Optional.empty(),
                100,
                new ZLinkPlacementCapacity(
                    new systems.zlink.framework.locations.ZLinkCapacityUsage(0, 0, 10_000),
                    new systems.zlink.framework.locations.ZLinkCapacityUsage(0, 0, 128),
                    List.of()),
                new ZLinkActivationConcurrency(0, 128),
                Optional.empty(),
                ZLinkFrameworkRuntimeState.SERVING,
                "cluster-a",
                "mesh-owner-a",
                9,
                Instant.parse("2024-07-15T00:00:00Z"));

        assertEquals(
            hash.path("json").asText(),
            ZLinkLocationDescriptorCodec.serializeMeshNode(descriptor));
        JsonNode immutableDigest = root.path("immutableDigest");
        assertEquals(
            immutableDigest.path("preimage").asText(),
            ZLinkLocationDescriptorCodec.meshNodeImmutablePreimage(
                descriptor));
        assertEquals(
            immutableDigest.path("sha256LowerHex").asText(),
            ZLinkLocationDescriptorCodec.meshNodeImmutableFingerprint(
                descriptor));
        assertEquals(
            Long.toString(descriptor.lifecycleGeneration()),
            hash.path("gen").asText());
    }

    private static Path fixturePath() {
        Path current = Path.of("").toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve("framework/testdata/location/redis/actor-location-v2.json");
            if (Files.exists(candidate)) {
                return candidate;
            }
            candidate = current.resolve("testdata/location/redis/actor-location-v2.json");
            if (Files.exists(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException("Could not find framework/testdata/location/redis/actor-location-v2.json");
    }

    private static Path descriptorFixturePath() {
        Path current = Path.of("").toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "framework/testdata/location/redis/"
                    + "mesh-node-descriptor-v1.json");
            if (Files.exists(candidate)) {
                return candidate;
            }
            candidate = current.resolve(
                "testdata/location/redis/mesh-node-descriptor-v1.json");
            if (Files.exists(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            "Could not find mesh-node-descriptor-v1.json");
    }

}
