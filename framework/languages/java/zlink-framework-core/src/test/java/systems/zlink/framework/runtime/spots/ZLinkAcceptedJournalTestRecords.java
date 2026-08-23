package systems.zlink.framework.runtime.spots;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceFrozenRecordCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;

final class ZLinkAcceptedJournalTestRecords {
    private ZLinkAcceptedJournalTestRecords() {
    }

    static byte[] actor(
        String actorId,
        long replyRouteId,
        String packetName,
        Map<String, String> metadata,
        byte[] payload) {
        return actor(
            actorId,
            replyRouteId,
            metadata,
            new ZLinkServiceM6AWireCodec.ApplicationPayload(
                packetName,
                "application/zlink-framework-json-v1",
                payload));
    }

    static byte[] actorMultipart(
        String actorId,
        ZLinkStreamHeader header,
        byte[] payload) {
        try (Message headerPart = Message.from(
                 ZLinkStreamHeaderCodec.encode(header));
             Message payloadPart = Message.from(payload)) {
            return actor(
                actorId,
                header.requestSequence().orElse(0L),
                Map.of(),
                ZLinkServiceM6AWireCodec.encodeFrameworkMultipart(
                    java.util.List.of(headerPart, payloadPart)));
        }
    }

    private static byte[] actor(
        String actorId,
        long replyRouteId,
        Map<String, String> metadata,
        ZLinkServiceM6AWireCodec.ApplicationPayload applicationPayload) {
        RoutingId nodeRid = RoutingId.from("journal-node");
        var owner = new ZLinkInternalMeshNode.PeerAuthorityFence(
            nodeRid, 1, "journal-owner", 1);
        var operation = new ZLinkServiceM6BWireCodec.ActorMessage(
            replyRouteId != 0,
            metadata.isEmpty() ? 0 : 1,
            replyRouteId == 0 ? null : replyRouteId,
            1,
            2,
            0,
            null,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(nodeRid, actorId, 1),
                1,
                1,
                1),
            new ZLinkServiceM6BWireCodec.BoundSessionTail(
                RoutingId.from("journal-session"), 1, 1));
        var wire = new ZLinkServiceM6AWireCodec();
        return ZLinkServiceFrozenRecordCodec.encodeActor(
            owner,
            owner,
            operation,
            metadata(metadata),
            wire.encodeApplicationPayload(applicationPayload));
    }

    static byte[] spot(
        String sourceSpotId,
        String targetSpotId,
        long replyRouteId,
        String packetName,
        Map<String, String> metadata,
        byte[] payload) {
        return spot(
            sourceSpotId,
            targetSpotId,
            replyRouteId,
            metadata,
            new ZLinkServiceM6AWireCodec.ApplicationPayload(
                packetName,
                "application/zlink-framework-json-v1",
                payload));
    }

    static byte[] spotMultipart(
        String sourceSpotId,
        String targetSpotId,
        long replyRouteId,
        java.util.List<Message> parts) {
        return spot(
            sourceSpotId,
            targetSpotId,
            replyRouteId,
            Map.of(),
            ZLinkServiceM6AWireCodec.encodeFrameworkMultipart(parts));
    }

    private static byte[] spot(
        String sourceSpotId,
        String targetSpotId,
        long replyRouteId,
        Map<String, String> metadata,
        ZLinkServiceM6AWireCodec.ApplicationPayload applicationPayload) {
        RoutingId nodeRid = RoutingId.from("journal-node");
        var owner = new ZLinkInternalMeshNode.PeerAuthorityFence(
            nodeRid, 1, "journal-owner", 1);
        var operation = new ZLinkServiceM6BWireCodec.SpotMessage(
            replyRouteId != 0,
            metadata.isEmpty() ? 0 : 1,
            replyRouteId == 0 ? null : replyRouteId,
            1,
            2,
            0,
            sourceSpotId,
            new ZLinkServiceM6BWireCodec.SpotRouteFence(
                targetSpotId, 1, nodeRid, 1, 1, 1));
        var wire = new ZLinkServiceM6AWireCodec();
        return ZLinkServiceFrozenRecordCodec.encodeSpot(
            owner,
            owner,
            operation,
            metadata(metadata),
            wire.encodeApplicationPayload(applicationPayload));
    }

    private static byte[] metadata(Map<String, String> metadata) {
        if (metadata.isEmpty()) {
            return new byte[0];
        }
        try {
            var bytes = new ByteArrayOutputStream();
            var output = new DataOutputStream(bytes);
            output.writeByte(1);
            output.writeByte(metadata.size());
            for (var entry : metadata.entrySet()) {
                byte[] key = entry.getKey().getBytes(StandardCharsets.UTF_8);
                byte[] value = entry.getValue().getBytes(StandardCharsets.UTF_8);
                output.writeByte(key.length);
                output.write(key);
                output.writeShort(value.length);
                output.write(value);
            }
            output.flush();
            return bytes.toByteArray();
        } catch (IOException impossible) {
            throw new IllegalStateException(impossible);
        }
    }
}
