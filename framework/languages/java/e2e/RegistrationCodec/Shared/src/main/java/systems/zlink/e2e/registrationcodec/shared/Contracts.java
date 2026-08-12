package systems.zlink.e2e.registrationcodec.shared;

import java.util.List;
import systems.zlink.framework.handlers.ZLinkPacket;

public final class Contracts {
    public static final String CHANNEL = "registration.codec.api";
    public static final String AUTO_GROUP = "registration-codec-auto";
    public static final String ATTR_GROUP = "registration-codec-attr";

    private Contracts() {
    }

    @ZLinkPacket("EchoAutoReq")
    public record EchoAutoReq(String value) {
    }

    @ZLinkPacket("EchoAutoMsg")
    public record EchoAutoMsg(String value) {
    }

    @ZLinkPacket("EchoAttrReq")
    public record EchoAttrReq(String value) {
    }

    @ZLinkPacket("EchoAttrMsg")
    public record EchoAttrMsg(String value) {
    }

    @ZLinkPacket("EchoManualReq")
    public record EchoManualReq(String value) {
    }

    @ZLinkPacket("EchoManualMsg")
    public record EchoManualMsg(String value) {
    }

    @ZLinkPacket("DiLifecycleReq")
    public record DiLifecycleReq(String value) {
    }

    public record DiLifecycleRes(
        String value,
        int scopedId,
        int singletonId,
        int disposedCount) {
    }

    @ZLinkPacket("JsonEchoReq")
    public record JsonEchoReq(String value) {
    }

    @ZLinkPacket("JsonEchoMsg")
    public record JsonEchoMsg(String value) {
    }

    @ZLinkPacket("JsonGoldenReq")
    public record JsonGoldenReq(String value) {
    }

    public record JsonGoldenRes(
        String displayName,
        String status,
        long balance,
        byte[] payload,
        int score,
        double ratio,
        String optionalNote,
        String contentType) {
    }

    @ZLinkPacket("PackedEchoReq")
    public record PackedEchoReq(String value) {
    }

    public record PackedEchoRes(String value) {
    }

    @ZLinkPacket("PackedEchoMsg")
    public record PackedEchoMsg(String value) {
    }

    public record EchoRes(String value, String handler) {
    }

    public record CodecScenarioRes(
        EchoRes json,
        String protobufValue,
        String messagePackValue) {
    }

    public record CodecMismatchProbeRes(
        boolean rejected,
        String errorType,
        String replyValue) {
    }

    public record EvidenceEntry(String marker, String packetName, String value) {
    }

    public record EvidenceSnapshot(List<EvidenceEntry> entries) {
    }
}
