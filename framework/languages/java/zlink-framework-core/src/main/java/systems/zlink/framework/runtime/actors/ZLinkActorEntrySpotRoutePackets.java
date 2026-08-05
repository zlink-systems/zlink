package systems.zlink.framework.runtime.actors;

import java.nio.charset.StandardCharsets;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkConfigurationException;

public final class ZLinkActorEntrySpotRoutePackets {
    public static final String JOIN_ENTRY_SPOT_PACKET_NAME = "__zlink.actor.joinEntrySpot";

    private ZLinkActorEntrySpotRoutePackets() {
    }

    public static Message encodeJoinRequest(
        String actorId,
        String actorType,
        RoutingId sourceNodeRid,
        long sourceGeneration) {
        return Message.from(String.join(
            "\n",
            actorId,
            actorType,
            sourceNodeRid.toString(),
            Long.toUnsignedString(sourceGeneration)).getBytes(StandardCharsets.UTF_8));
    }

    public static JoinRequest decodeJoinRequest(Message message) {
        String[] fields = message.toUtf8String().split("\n", -1);
        if (fields.length != 4 || fields[0].isBlank() || fields[1].isBlank()) {
            throw new ZLinkConfigurationException("invalid actor Entry Spot route join request");
        }
        return new JoinRequest(
            fields[0],
            fields[1],
            RoutingId.from(fields[2]),
            Long.parseUnsignedLong(fields[3]));
    }

    public static Message encodeJoinReply(
        String actorId,
        String actorType,
        RoutingId targetNodeRid,
        long actorGeneration) {
        return Message.from(String.join(
            "\n",
            actorId,
            actorType,
            targetNodeRid.toString(),
            Long.toUnsignedString(actorGeneration)).getBytes(StandardCharsets.UTF_8));
    }

    public static JoinReply decodeJoinReply(Message message) {
        String[] fields = message.toUtf8String().split("\n", -1);
        if (fields.length != 4 || fields[0].isBlank() || fields[1].isBlank()) {
            throw new ZLinkConfigurationException("invalid actor Entry Spot route join reply");
        }
        return new JoinReply(
            fields[0],
            fields[1],
            RoutingId.from(fields[2]),
            Long.parseUnsignedLong(fields[3]));
    }

    public record JoinRequest(
        String actorId,
        String actorType,
        RoutingId sourceNodeRid,
        long sourceGeneration) {
    }

    public record JoinReply(
        String actorId,
        String actorType,
        RoutingId targetNodeRid,
        long actorGeneration) {
    }
}
