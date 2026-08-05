package systems.zlink.framework.testkit;

import static org.junit.jupiter.api.Assertions.assertEquals;

import com.google.protobuf.StringValue;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.codecs.msgpack.ZLinkMessagePackCodec;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamCodec;
import systems.zlink.stream.connector.ZLinkStreamCompression;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload;
import systems.zlink.stream.connector.ZLinkStreamJson;

final class ConnectorCodecContractTest {
    @Test
    void jsonMsgpackProtobufTypedHelperRoundtrip() {
        assertCodecRoundtrip(
            "JsonPacket",
            ZLinkStreamCodec.JSON,
            ZLinkStreamJson.encode("JsonPacket", "json-body"),
            "json-body",
            payload -> ZLinkStreamJson.decode(payload, String.class));
        assertCodecRoundtrip(
            "MsgpackPacket",
            ZLinkStreamCodec.MESSAGE_PACK,
            ZLinkMessagePackCodec.defaultCodec().encode("MsgpackPacket", "msgpack-body"),
            "msgpack-body",
            payload -> ZLinkMessagePackCodec.defaultCodec().decode(payload, String.class));
        assertCodecRoundtrip(
            "ProtobufPacket",
            ZLinkStreamCodec.PROTOBUF,
            ZLinkProtobufCodec.defaultCodec().encode("ProtobufPacket", "protobuf-body"),
            "protobuf-body",
            payload -> ZLinkProtobufCodec.defaultCodec().decode(payload, String.class));
    }

    @Test
    void protobufTypedHelperUsesMessageLiteBytes() throws Exception {
        StringValue original = StringValue.of("profile:42");
        ZLinkStreamEncodedPayload encoded =
            ZLinkProtobufCodec.defaultCodec().encode("StringValue", original);
        try {
            assertEquals("StringValue", encoded.packetName());
            assertEquals(ZLinkStreamCodec.PROTOBUF, encoded.codec());
            assertEquals(original, StringValue.parseFrom(encoded.payload().toByteArray()));
            assertEquals(original, ZLinkProtobufCodec.defaultCodec().decode(encoded, StringValue.class));
        } finally {
            encoded.payload().close();
        }
    }

    @Test
    void jsonTypedHelperUsesConnectorSendRequestAndOnSurface() throws Exception {
        try (TcpServer server = new TcpServer()) {
            ZLinkStreamConnector connector =
                ZLinkStreamConnectorFactory.create(options(server.endpoint()));
            try {
            List<String> handled = new ArrayList<>();
            ZLinkStreamJson.on(connector, "String", String.class, message -> {
                handled.add(message.payload());
                return java.util.concurrent.CompletableFuture.completedFuture(null);
            });

            connector.connect().submit().toCompletableFuture().join();
            ZLinkStreamJson.send(connector, "hello")
                .compress()
                .submit();
            Frame sent = server.readFrame();
            assertEquals(1, sent.kind());
            assertEquals(1, sent.codec());
            assertEquals("String", sent.name());

            server.sendFrame(new Frame(
                1,
                1,
                null,
                "String",
                "\"server\"".getBytes(StandardCharsets.UTF_8)));
            awaitPendingDispatch(connector);
            connector.dispatch().submit().toCompletableFuture().join();

            var replyFuture = ZLinkStreamJson.request(connector, "reply")
                .compress()
                .submit()
                .toCompletableFuture();
            Frame request = server.readFrame();
            assertEquals(2, request.kind());
            assertEquals(1, request.codec());
            assertEquals("String", request.name());
            server.sendFrame(new Frame(
                3,
                1,
                request.requestSeq(),
                "",
                "\"reply\"".getBytes(StandardCharsets.UTF_8)));

            ZLinkStreamEncodedPayload reply = replyFuture.join();
            try {
                assertEquals("reply", ZLinkStreamJson.decode(reply, String.class));
            } finally {
                reply.payload().close();
            }

            assertEquals(List.of("server"), handled);
            } finally {
                connector.close().submit().toCompletableFuture().join();
            }
        }
    }

    private static void assertCodecRoundtrip(
        String packetName,
        ZLinkStreamCodec codec,
        ZLinkStreamEncodedPayload encoded,
        String expected,
        java.util.function.Function<ZLinkStreamEncodedPayload, String> decode) {
        try {
            assertEquals(packetName, encoded.packetName());
            assertEquals(codec, encoded.codec());
            assertEquals(0, encoded.metadata().size());
            assertEquals(expected, decode.apply(encoded));
        } finally {
            encoded.payload().close();
        }
    }

    private static ZLinkStreamConnectorOptions options(URI endpoint) {
        return new ZLinkStreamConnectorOptions(
            endpoint,
            ZLinkStreamDispatchMode.MANUAL,
            Duration.ofSeconds(1),
            1,
            Duration.ofSeconds(1),
            64 * 1024,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            true,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            false,
            ZLinkStreamCompression.LZ4);
    }

    private static void awaitPendingDispatch(ZLinkStreamConnector connector) {
        long deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < deadline) {
            if (connector.pendingDispatchCount() > 0) {
                return;
            }
            Thread.onSpinWait();
        }
        throw new AssertionError("connector did not queue inbound dispatch within 5s");
    }

    private record Frame(
        int kind,
        int codec,
        Long requestSeq,
        String name,
        byte[] payload) {
    }

    private static final class TcpServer implements AutoCloseable {
        private final ServerSocket server;
        private final java.util.concurrent.BlockingQueue<Socket> sockets =
            new java.util.concurrent.LinkedBlockingQueue<>();
        private final Thread acceptThread;
        private Socket current;

        TcpServer() throws IOException {
            server = new ServerSocket(0);
            acceptThread = new Thread(() -> {
                while (!server.isClosed()) {
                    try {
                        sockets.add(server.accept());
                    } catch (IOException ex) {
                        if (!server.isClosed()) {
                            throw new RuntimeException(ex);
                        }
                    }
                }
            }, "zlink-connector-codec-contract-server");
            acceptThread.setDaemon(true);
            acceptThread.start();
        }

        URI endpoint() {
            return URI.create("tcp://127.0.0.1:" + server.getLocalPort());
        }

        Frame readFrame() throws Exception {
            DataInputStream input = new DataInputStream(socket().getInputStream());
            int headerLength = input.readUnsignedShort();
            int payloadLength = input.readInt();
            byte[] header = input.readNBytes(headerLength);
            byte[] payload = input.readNBytes(payloadLength);
            Header decoded = decodeHeader(header);
            return new Frame(
                decoded.kind(),
                decoded.codec(),
                decoded.requestSeq(),
                decoded.name(),
                payload);
        }

        void sendFrame(Frame frame) throws Exception {
            byte[] header = encodeHeader(frame);
            byte[] encoded = ByteBuffer.allocate(6 + header.length + frame.payload().length)
                .putShort((short) header.length)
                .putInt(frame.payload().length)
                .put(header)
                .put(frame.payload())
                .array();
            DataOutputStream output = new DataOutputStream(socket().getOutputStream());
            output.write(encoded);
            output.flush();
        }

        @Override
        public void close() throws Exception {
            if (current != null) {
                current.close();
            }
            server.close();
        }

        private Socket socket() throws InterruptedException {
            if (current != null && !current.isClosed()) {
                return current;
            }
            current = sockets.poll(5, java.util.concurrent.TimeUnit.SECONDS);
            if (current == null) {
                throw new AssertionError("client did not connect within 5s");
            }
            return current;
        }

        private static byte[] encodeHeader(Frame frame) {
            byte[] name = frame.name().getBytes(StandardCharsets.UTF_8);
            int flags = 0;
            if (frame.requestSeq() != null) {
                flags |= 0x01;
            }
            ByteBuffer buffer = ByteBuffer.allocate(
                4
                    + (frame.requestSeq() == null ? 0 : 8)
                    + 1
                    + name.length);
            buffer.put((byte) 0xF2);
            buffer.put((byte) frame.kind());
            buffer.put((byte) frame.codec());
            buffer.put((byte) flags);
            if (frame.requestSeq() != null) {
                buffer.putLong(frame.requestSeq());
            }
            buffer.put((byte) name.length);
            buffer.put(name);
            return buffer.array();
        }

        private static Header decodeHeader(byte[] header) {
            ByteBuffer buffer = ByteBuffer.wrap(header);
            assertEquals(0xF2, Byte.toUnsignedInt(buffer.get()));
            int kind = Byte.toUnsignedInt(buffer.get());
            int codec = Byte.toUnsignedInt(buffer.get());
            int flags = Byte.toUnsignedInt(buffer.get());
            Long requestSeq = (flags & 0x01) == 0 ? null : buffer.getLong();
            int nameLength = Byte.toUnsignedInt(buffer.get());
            byte[] name = new byte[nameLength];
            buffer.get(name);
            return new Header(
                kind,
                codec,
                requestSeq,
                new String(name, StandardCharsets.UTF_8));
        }

        private record Header(
            int kind,
            int codec,
            Long requestSeq,
            String name) {
        }
    }
}
