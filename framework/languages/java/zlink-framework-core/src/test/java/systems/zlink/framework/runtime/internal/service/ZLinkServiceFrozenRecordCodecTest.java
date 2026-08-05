package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;

final class ZLinkServiceFrozenRecordCodecTest {
    private final ZLinkServiceM6AWireCodec application =
        new ZLinkServiceM6AWireCodec();

    @Test
    void spotRecordPreservesAcceptedSourceFenceAndReplyRoute() {
        RoutingId sourceRid = RoutingId.from("frozen-source");
        RoutingId targetRid = RoutingId.from("frozen-target");
        var operation = new ZLinkServiceM6BWireCodec.SpotMessage(
            true,
            0,
            71L,
            73,
            79,
            0,
            "source-spot",
            new ZLinkServiceM6BWireCodec.SpotRouteFence(
                "target-spot", 83, targetRid, 89, 97, 101));
        byte[] metadata = new byte[] {
            1, 1, 3, 'k', 'e', 'y', 0, 5,
            'v', 'a', 'l', 'u', 'e'};
        byte[] encoded = ZLinkServiceFrozenRecordCodec.encodeSpot(
            fence(sourceRid, 11, "source-owner", 13),
            fence(targetRid, 89, "target-owner", 101),
            operation,
            metadata,
            application.encodeApplicationPayload(
                new ZLinkServiceM6AWireCodec.ApplicationPayload(
                    "packet", "application/json", new byte[] {1, 2, 3})));

        var decoded = ZLinkServiceFrozenRecordCodec.decodeSpot(encoded);
        assertEquals(sourceRid, decoded.sourceNodeRid());
        assertEquals("source-spot", decoded.sourceSpotId().orElseThrow());
        assertEquals(71, decoded.replyRouteId().orElseThrow());
        assertEquals("target-spot", decoded.targetSpotId());
        assertEquals("packet", decoded.packetName());
        assertArrayEquals(metadata, decoded.metadataFrame());
        assertArrayEquals(new byte[] {1, 2, 3}, decoded.payload());
    }

    @Test
    void boundActorRecordPreservesSessionIdentityAndCanonicalPayload() {
        RoutingId sourceRid = RoutingId.from("session-owner");
        RoutingId targetRid = RoutingId.from("actor-owner");
        RoutingId sessionRid = RoutingId.from("session");
        var operation = new ZLinkServiceM6BWireCodec.ActorMessage(
            false,
            0,
            null,
            103,
            107,
            0,
            null,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(targetRid, "actor-1", 109),
                113,
                127,
                131),
            new ZLinkServiceM6BWireCodec.BoundSessionTail(
                sessionRid, 131, 137));
        byte[] encoded = ZLinkServiceFrozenRecordCodec.encodeActor(
            fence(sourceRid, 139, "session-owner-id", 149),
            fence(targetRid, 113, "actor-owner-id", 151),
            operation,
            new byte[0],
            application.encodeApplicationPayload(
                new ZLinkServiceM6AWireCodec.ApplicationPayload(
                    "actor-packet",
                    "application/octet-stream",
                    new byte[] {5, 8, 13})));

        var decoded = ZLinkServiceFrozenRecordCodec.decodeActor(encoded);
        assertEquals("actor-1", decoded.actorId());
        assertEquals(sourceRid, decoded.sourceNodeRid());
        assertEquals(sessionRid, decoded.sourceSessionRid());
        assertTrue(decoded.replyRouteId().isEmpty());
        assertEquals("actor-packet", decoded.packetName());
        assertArrayEquals(new byte[] {5, 8, 13}, decoded.payload());
    }

    private static ZLinkInternalMeshNode.PeerAuthorityFence fence(
        RoutingId rid,
        long nodeGeneration,
        String ownerId,
        long ownerLeaseGeneration) {
        return new ZLinkInternalMeshNode.PeerAuthorityFence(
            rid, nodeGeneration, ownerId, ownerLeaseGeneration);
    }
}
