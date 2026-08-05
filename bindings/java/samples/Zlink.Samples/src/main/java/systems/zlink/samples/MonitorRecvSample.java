package systems.zlink.samples;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.sockets.PairSocket;
public final class MonitorRecvSample {
    public static void main(String[] args) {
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

            var serverEvent = serverMonitor.recv();
            var clientEvent = clientMonitor.recv();
            if (serverEvent.event() != MonitorEventType.CONNECTION_READY
                || clientEvent.event() != MonitorEventType.CONNECTION_READY) {
                throw new IllegalStateException("expected connection-ready events");
            }
            System.out.println("[monitor/recv] recv: \"connection-ready\"");
        }
    }
}
