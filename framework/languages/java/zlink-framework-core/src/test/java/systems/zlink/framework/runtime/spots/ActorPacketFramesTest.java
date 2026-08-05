package systems.zlink.framework.runtime.spots;

import com.fasterxml.jackson.databind.ObjectMapper;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ActorPacketFramesTest {
    @Test
    void streamActorPacketHeaderKeepsMetadataAndCompressionFlagForForwarding() {
        ZLinkStreamHeader request = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.of(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED),
            Optional.of(7L),
            "ActorReq",
            Map.of("trace-id", "trace-1"));
        try (Message headerPart = Message.from(ZLinkStreamHeaderCodec.encode(request))) {
            ActorPacketFrames.Header decoded = ActorPacketFrames.decode(actorReceived(headerPart));
            ZLinkStreamHeader forwarded = decoded.toRequestHeader();

            assertEquals(Map.of("trace-id", "trace-1"), forwarded.metadata());
            assertTrue(forwarded.flags().contains(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED));
            assertEquals(Optional.of(7L), forwarded.requestSequence());
        }
    }

    @Test
    void actorReplyDoesNotCopyMetadataOrClaimUnappliedCompression() {
        ZLinkStreamHeader request = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.of(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED),
            Optional.of(7L),
            "ActorReq",
            Map.of("trace-id", "trace-1"));
        try (Message headerPart = Message.from(ZLinkStreamHeaderCodec.encode(request));
             Message payload = Message.from("reply".getBytes());
             Message frame = ActorPacketFrames.encodeReply(
                 ActorPacketFrames.decode(actorReceived(headerPart)),
                 payload)) {
            DecodedFrame decoded = decodeFrame(frame);

            assertEquals(ZLinkStreamMessageKind.RESPONSE, decoded.header().kind());
            assertEquals(Map.of(), decoded.header().metadata());
            assertFalse(decoded.header().flags().contains(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED));
            assertEquals("reply", new String(decoded.body(), StandardCharsets.UTF_8));
        }
    }

    @Test
    void streamActorErrorUsesTheCommonJsonEnvelope() {
        ZLinkStreamHeader request = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(7L),
            "ActorReq",
            Map.of());
        try (Message headerPart = Message.from(ZLinkStreamHeaderCodec.encode(request));
             Message frame = ActorPacketFrames.encodeError(
                 ActorPacketFrames.decode(actorReceived(headerPart)),
                 new IllegalStateException("handler failed"))) {
            DecodedFrame decoded = decodeFrame(frame);

            assertEquals(ZLinkStreamMessageKind.ERROR, decoded.header().kind());
            assertEquals(ZLinkStreamCodec.JSON, decoded.header().codec());
            try {
                var json = new ObjectMapper().readTree(decoded.body());
                assertEquals("IllegalStateException", json.get("code").asText());
                assertEquals("handler failed", json.get("message").asText());
            } catch (java.io.IOException failure) {
                throw new AssertionError("error payload is not JSON", failure);
            }
        }
    }

    @Test
    void streamActorFrameworkErrorUsesTheCommonWireCode() {
        ZLinkStreamHeader request = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(7L),
            "ActorReq",
            Map.of());
        try (Message headerPart = Message.from(ZLinkStreamHeaderCodec.encode(request));
             Message frame = ActorPacketFrames.encodeError(
                 ActorPacketFrames.decode(actorReceived(headerPart)),
                 new ZLinkFrameworkException(
                     ZLinkFrameworkErrorKind.NOT_FOUND,
                     "actor was not found"))) {
            DecodedFrame decoded = decodeFrame(frame);

            try {
                var json = new ObjectMapper().readTree(decoded.body());
                assertEquals("NotFound", json.get("code").asText());
                assertEquals("actor was not found", json.get("message").asText());
            } catch (java.io.IOException failure) {
                throw new AssertionError("error payload is not JSON", failure);
            }
        }
    }

    private static ZLinkBackendActorReceived actorReceived(Message headerPart) {
        return new ZLinkBackendActorReceived(
            new ZLinkBackendActorRef(RoutingId.from("actor-node"), "actor-a", 1),
            RoutingId.from("session-node"),
            RoutingId.from("session"),
            Optional.empty(),
            42,
            0,
            Message.from(headerPart),
            true);
    }

    private static DecodedFrame decodeFrame(Message frame) {
        ZLinkStreamFrameCodec.DecodedFrame decoded = ZLinkStreamFrameCodec
            .tryDecode(frame.toByteArray())
            .orElseThrow();
        return new DecodedFrame(
            ZLinkStreamHeaderCodec.decodeOrPlain(decoded.header()),
            decoded.body());
    }

    private record DecodedFrame(ZLinkStreamHeader header, byte[] body) {
    }
}
