// W-2 byte-equivalence proof: generated pilot codecs vs hand codecs (Java).
// Declared in the channels package on purpose, to reach the real
// package-private encodeLivenessProbe/encodeLivenessAck hand codecs
// directly - not a copied/hardcoded expected-bytes string.
package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;
import java.util.HexFormat;

public class WEquivW2 {
    static boolean allEqual = true;

    static void check(String name, byte[] hand, byte[] gen) {
        String h = HexFormat.of().formatHex(hand);
        String g = HexFormat.of().formatHex(gen);
        boolean equal = h.equals(g);
        if (!equal) allEqual = false;
        System.out.println(name + ": " + (equal ? "IDENTICAL" : "DIFFERS"));
        System.out.println("  hand: " + h);
        System.out.println("  gen : " + g);
    }

    public static void main(String[] args) throws Exception {
        // livenessProbe(5) / livenessAck(6): package-private hand codecs in
        // ZLinkClientServerServiceWire, reached because this class shares
        // its package.
        check("livenessProbe(5)",
            ZLinkClientServerServiceWire.encodeLivenessProbe(42L),
            ServiceWirePilotCodec.encodeLivenessProbe5(new ServiceWirePilotCodec.LivenessProbe5(42L)));
        check("livenessAck(6)",
            ZLinkClientServerServiceWire.encodeLivenessAck(42L),
            ServiceWirePilotCodec.encodeLivenessAck6(new ServiceWirePilotCodec.LivenessAck6(42L)));

        var m6a = new ZLinkServiceM6AWireCodec();
        check("nodeSend(16)",
            m6a.encodeNodeSendHeader(0),
            ServiceWirePilotCodec.encodeNodeSend16());
        check("nodeRequest(17)",
            m6a.encodeNodeRequestHeader(7L, 0),
            ServiceWirePilotCodec.encodeNodeRequest17(new ServiceWirePilotCodec.NodeRequest17(7L)));
        check("channelSend(18)",
            m6a.encodeChannelSendHeader("lobby", 0),
            ServiceWirePilotCodec.encodeChannelSend18(new ServiceWirePilotCodec.ChannelSend18("lobby")));
        check("channelRequest(19)",
            m6a.encodeChannelRequestHeader(7L, "lobby", 0),
            ServiceWirePilotCodec.encodeChannelRequest19(new ServiceWirePilotCodec.ChannelRequest19(7L, "lobby")));

        var m6b = new ZLinkServiceM6BWireCodec();
        check("logicalMulticast(23)",
            m6b.encodeLogicalMulticastHeader(0, "lobby", "topicA", "spot1"),
            ServiceWirePilotCodec.encodeLogicalMulticast23(
                new ServiceWirePilotCodec.LogicalMulticast23("lobby", "topicA", "spot1")));

        // actorLookup(26) / actorDestroy(27): no Java hand codec exists for
        // either (confirmed: ZLinkServiceM6BWireCodec has no
        // encodeActorLookup*/encodeActorDestroy* method, and
        // ZLinkServiceWireCodec.java is a generic prefix+opaque-body
        // wrapper, not a field-level codec) - generated only, not
        // equivalence-checked.
        System.out.println("actorLookup(26): NO HAND CODEC (new capability) - generated only");
        System.out.println("actorDestroy(27): NO HAND CODEC (new capability) - generated only");

        System.exit(allEqual ? 0 : 1);
    }
}
