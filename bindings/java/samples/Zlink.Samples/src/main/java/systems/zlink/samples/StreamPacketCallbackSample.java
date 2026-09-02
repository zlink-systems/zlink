package systems.zlink.samples;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.StreamPacket;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.StreamRecvMode;
import systems.zlink.contracts.sockets.StreamSocket;
import java.nio.charset.StandardCharsets;

public final class StreamPacketCallbackSample {
    public static void main(String[] args) throws Exception {
// --8<-- [start:doc]
        SampleSupport.ensureNative();
        String endpoint = SampleSupport.tcpEndpoint();
        try (Context ctx = Zlink.createContext();
             StreamSocket server = ctx.createStreamSocket();
             var monitor = server.monitorOpen(
                 systems.zlink.contracts.eventing.MonitorEventType.ACCEPTED,
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY)) {
            server.options().recvMode(StreamRecvMode.PACKET);
            server.bind(endpoint);
            try (var rawClient = SampleSupport.connectRawTcp(endpoint)) {
                SampleSupport.waitStreamConnected(monitor);

                try (StreamPacket packet = new StreamPacket()) {
                    byte[] payload = SampleSupport.STREAM_PAYLOAD
                        .getBytes(StandardCharsets.UTF_8);
                    byte[] frame = frameBytes(payload);
                    SampleSupport.sendRawTcp(rawClient, frame);
                    if (!server.recvPacket(packet, RecvFlags.NONE)) {
                        throw new IllegalStateException("missing stream packet");
                    }
                    var routingId = packet.routingId().orElseThrow();
                    String value = packet.body().toUtf8String();
                    if (!SampleSupport.STREAM_PAYLOAD.equals(value)) {
                        throw new IllegalStateException("unexpected stream payload");
                    }
                    try (Message replyHeader = Message.from(new byte[0]);
                         Message replyBody = Message.from(
                             SampleSupport.STREAM_PAYLOAD);
                         Message reply = frame(replyHeader, replyBody)) {
                        server.send(routingId)
                            .message(reply)
                            .submit().toCompletableFuture().join();
                    }

                    byte[] echoedFrame = SampleSupport.recvExactRawTcp(
                        rawClient, frame.length);
                String echoed = new String(
                    java.util.Arrays.copyOfRange(echoedFrame, 6,
                        echoedFrame.length),
                    StandardCharsets.UTF_8);
                if (!SampleSupport.STREAM_PAYLOAD.equals(echoed)) {
                    throw new IllegalStateException("unexpected stream echo: " + echoed);
                }
                System.out.println("[stream/packet-callback] send: \"" +
                    SampleSupport.STREAM_PAYLOAD + "\" -> recv: \"" +
                    echoed + "\"");
                }
            }
        }
// --8<-- [end:doc]
    }

    private static byte[] frameBytes(byte[] body) {
        byte[] frame = new byte[6 + body.length];
        frame[2] = (byte) (body.length >>> 24);
        frame[3] = (byte) (body.length >>> 16);
        frame[4] = (byte) (body.length >>> 8);
        frame[5] = (byte) body.length;
        System.arraycopy(body, 0, frame, 6, body.length);
        return frame;
    }

    private static Message frame(Message header, Message body) {
        byte[] headerBytes = header.toByteArray();
        byte[] bodyBytes = body.toByteArray();
        byte[] frame = new byte[6 + headerBytes.length + bodyBytes.length];
        frame[0] = (byte) (headerBytes.length >>> 8);
        frame[1] = (byte) headerBytes.length;
        frame[2] = (byte) (bodyBytes.length >>> 24);
        frame[3] = (byte) (bodyBytes.length >>> 16);
        frame[4] = (byte) (bodyBytes.length >>> 8);
        frame[5] = (byte) bodyBytes.length;
        System.arraycopy(headerBytes, 0, frame, 6, headerBytes.length);
        System.arraycopy(bodyBytes, 0, frame, 6 + headerBytes.length,
            bodyBytes.length);
        return Message.from(frame);
    }
}
