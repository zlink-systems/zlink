package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkJavaRawMeshNodeApplicationPayloadTest {
    @Test
    void typedActorRequestUsesFrameworkMultipartAndRestoresAllParts() {
        byte[] body = "{\"actorId\":\"actor-1\"}".getBytes(
            java.nio.charset.StandardCharsets.UTF_8);
        ZLinkStreamHeader request = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(7L),
            "JoinTargetReq",
            Map.of());

        try (Message header = Message.from(ZLinkStreamHeaderCodec.encode(request));
             Message payload = Message.from(body)) {
            var wirePayload = ZLinkJavaRawMeshNode.applicationPayload(
                List.of(header, payload));

            assertEquals(
                systems.zlink.framework.runtime.protocol.ServiceWireConstants
                    .FRAMEWORK_MULTIPART_PACKET_NAME,
                wirePayload.packetName());
            List<Message> decoded =
                systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6AWireCodec.decodeFrameworkMultipart(
                        wirePayload);
            try {
                assertArrayEquals(
                    header.toByteArray(), decoded.getFirst().toByteArray());
                assertArrayEquals(body, decoded.get(1).toByteArray());
            } finally {
                decoded.forEach(Message::close);
            }

            List<Message> restored =
                ZLinkJavaRawMeshNode.applicationMessages(
                    wirePayload,
                    true,
                    7L);
            try {
                ZLinkStreamHeader restoredHeader =
                    ZLinkStreamHeaderCodec.decodeOrPlain(
                        restored.getFirst().toByteArray());
                assertEquals(
                    ZLinkStreamMessageKind.REQUEST,
                    restoredHeader.kind());
                assertEquals(Optional.of(7L), restoredHeader.requestSequence());
                assertEquals("JoinTargetReq", restoredHeader.packetName());
                assertEquals(ZLinkStreamCodec.JSON, restoredHeader.codec());
                assertArrayEquals(body, restored.get(1).toByteArray());
            } finally {
                restored.forEach(Message::close);
            }
        }
    }

    @Test
    void plainTwoPartMessageKeepsItsPacketName() {
        try (Message packetName = Message.from("plain.packet");
             Message payload = Message.from(new byte[] {1, 2, 3})) {
            var wirePayload = ZLinkJavaRawMeshNode.applicationPayload(
                List.of(packetName, payload));

            assertEquals(
                systems.zlink.framework.runtime.protocol.ServiceWireConstants
                    .FRAMEWORK_MULTIPART_PACKET_NAME,
                wirePayload.packetName());
            List<Message> decoded =
                systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6AWireCodec.decodeFrameworkMultipart(
                        wirePayload);
            try {
                assertEquals("plain.packet", decoded.getFirst().toUtf8String());
                assertArrayEquals(
                    new byte[] {1, 2, 3}, decoded.get(1).toByteArray());
            } finally {
                decoded.forEach(Message::close);
            }
        }
    }

    @Test
    void applicationMessagesPreserveTheSelectedStreamCodec() {
        ZLinkStreamHeader request = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.PROTOBUF,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(11L),
            "CustomReq",
            Map.of());
        try (Message header = Message.from(ZLinkStreamHeaderCodec.encode(request));
             Message payload = Message.from(new byte[] {4, 5, 6})) {
            List<Message> restored = ZLinkJavaRawMeshNode.applicationMessages(
                ZLinkJavaRawMeshNode.applicationPayload(
                    List.of(header, payload)),
                true,
                11L);
            try {
                ZLinkStreamHeader restoredHeader =
                    ZLinkStreamHeaderCodec.decodeOrPlain(
                        restored.getFirst().toByteArray());
                assertEquals(ZLinkStreamCodec.PROTOBUF, restoredHeader.codec());
                assertEquals(Optional.of(11L), restoredHeader.requestSequence());
            } finally {
                restored.forEach(Message::close);
            }
        }
    }
}
