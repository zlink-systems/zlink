package systems.zlink.samples;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.sockets.StreamRecvMode;
import java.nio.charset.StandardCharsets;

public final class StreamRecvSample {
    public static void main(String[] args) throws Exception {
// --8<-- [start:doc]
        SampleSupport.ensureNative();
        String endpoint = SampleSupport.tcpEndpoint();

        try (Context ctx = Zlink.createContext();
             StreamSocket server = ctx.createStreamSocket();
             var monitor = server.monitorOpen(
                 systems.zlink.contracts.eventing.MonitorEventType.ACCEPTED,
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY)) {
            server.options().recvMode(StreamRecvMode.RAW);
            server.bind(endpoint);
            try (var rawClient = SampleSupport.connectRawTcp(endpoint)) {
                SampleSupport.waitStreamConnected(monitor);

                byte[] payload =
                    SampleSupport.STREAM_PAYLOAD.getBytes(StandardCharsets.UTF_8);
                SampleSupport.sendRawTcp(rawClient, payload);

                try (systems.zlink.contracts.messaging.Received received = new systems.zlink.contracts.messaging.Received()) {
                    server.recv(received, systems.zlink.contracts.sockets.RecvFlags.NONE);
                    String value = SampleSupport.singleUtf8(received);
                    if (!SampleSupport.STREAM_PAYLOAD.equals(value)
                        || received.getRoutingId().isEmpty()) {
                        throw new IllegalStateException("unexpected stream delivery");
                    }

                    try (Message reply = Message.from(
                             SampleSupport.STREAM_PAYLOAD)) {
                        received.send().message(reply).submit();
                    }
                }

                String echoed = new String(
                    SampleSupport.recvExactRawTcp(rawClient, payload.length),
                    StandardCharsets.UTF_8);
                if (!SampleSupport.STREAM_PAYLOAD.equals(echoed)) {
                    throw new IllegalStateException("unexpected stream echo: " + echoed);
                }
                System.out.println("[stream/recv] send: \"" +
                    SampleSupport.STREAM_PAYLOAD + "\" \u2192 recv: \"" +
                    echoed + "\"");
            }
        }
// --8<-- [end:doc]
    }
}
