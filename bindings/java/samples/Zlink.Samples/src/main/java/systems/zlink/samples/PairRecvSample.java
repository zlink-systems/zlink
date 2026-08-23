package systems.zlink.samples;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
public final class PairRecvSample {
    public static void main(String[] args) {
// --8<-- [start:doc]
        SampleSupport.ensureNative();
        String endpoint = SampleSupport.tcpEndpoint();

        try (Context ctx = Zlink.createContext();
             PairSocket server = ctx.createPairSocket();
             PairSocket client = ctx.createPairSocket();
             var serverMonitor = server.monitorOpen(
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY);
             var clientMonitor = client.monitorOpen(
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY)) {
            server.bind(endpoint);
            client.connect(endpoint);
            SampleSupport.waitConnected(serverMonitor, clientMonitor);

            try (Message outbound = Message.from(SampleSupport.PAIR_PAYLOAD)) {
                client.send().message(outbound).submit()
                    .toCompletableFuture().join();
            }

            try (systems.zlink.contracts.messaging.Received received = new systems.zlink.contracts.messaging.Received()) {
                server.recv(received, systems.zlink.contracts.sockets.RecvFlags.NONE);
                String value = SampleSupport.singleUtf8(received);
                if (!SampleSupport.PAIR_PAYLOAD.equals(value)) {
                    throw new IllegalStateException("unexpected payload: " + value);
                }
                System.out.println("[pair/recv] send: \"" + SampleSupport.PAIR_PAYLOAD
                    + "\" \u2192 recv: \"" + value + "\"");
            }
        }
// --8<-- [end:doc]
    }
}
