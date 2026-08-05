package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationIdentity;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;

class ZLinkRedisLocationKeyCodecTest {
    @Test
    void descriptorKeysUseLengthPrefixedCanonicalSegments() {
        assertEquals(
            "4:mesh4:0123",
            ZLinkRedisLocationKeyCodec.encodeMeshNodeKey(
                new ZLinkMeshNodeDescriptorKey(
                    "mesh",
                    RoutingId.from(new byte[] {0x01, 0x23}))));
        assertEquals(
            "6:orders4:0123",
            ZLinkRedisLocationKeyCodec.encodeClientServerKey(
                new ZLinkClientServerServerDescriptorKey(
                    "orders",
                    RoutingId.from(new byte[] {0x01, 0x23}))));
        assertEquals(
            "6:events4:0123",
            ZLinkRedisLocationKeyCodec.encodeFanoutPublisherKey(
                new ZLinkFanoutPublisherDescriptorKey(
                    "events",
                    RoutingId.from(new byte[] {0x01, 0x23}))));
    }

    @Test
    void descriptorKeysRoundTripUnicodeWithoutNormalization() {
        var meshNode = new ZLinkMeshNodeDescriptorKey(
            "mésh",
            RoutingId.from(new byte[] {(byte) 0xff, 0x00}));
        var clientServer = new ZLinkClientServerServerDescriptorKey(
            "注文",
            RoutingId.from(new byte[] {0x01}));
        var publisher = new ZLinkFanoutPublisherDescriptorKey(
            "이벤트",
            RoutingId.from(new byte[] {0x02}));

        assertEquals(
            meshNode,
            ZLinkRedisLocationKeyCodec.decodeMeshNodeKey(
                ZLinkRedisLocationKeyCodec.encodeMeshNodeKey(meshNode)));
        assertEquals(
            clientServer,
            ZLinkRedisLocationKeyCodec.decodeClientServerKey(
                ZLinkRedisLocationKeyCodec.encodeClientServerKey(clientServer)));
        assertEquals(
            publisher,
            ZLinkRedisLocationKeyCodec.decodeFanoutPublisherKey(
                ZLinkRedisLocationKeyCodec.encodeFanoutPublisherKey(publisher)));
    }

    @Test
    void creationTerminalKeyUsesExactSourceLifecycleAndOperationId() {
        ZLinkRedisLocationKeys keys = new ZLinkRedisLocationKeys("P");

        assertEquals(
            "P:{zlink-location-v3}:creation-terminal:"
                + "2:0a0b:7:"
                + "00000000000000010000000000000002",
            keys.creationTerminalKey(
                new ZLinkCreationOperationIdentity(
                    RoutingId.from(new byte[] {0x0a, 0x0b}),
                    7,
                    1,
                    2)));
    }
}
