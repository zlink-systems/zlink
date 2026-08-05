package systems.zlink.samples;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
public final class DealerRouterRecvSample {
    public static void main(String[] args) {
// --8<-- [start:doc]
        SampleSupport.ensureNative();
        String endpoint = SampleSupport.tcpEndpoint();

        try (Context ctx = Zlink.createContext();
             RouterSocket router = ctx.createRouterSocket();
             DealerSocket dealer = ctx.createDealerSocket();
             var routerMonitor = router.monitorOpen(
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY);
             var dealerMonitor = dealer.monitorOpen(
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY)) {
            router.bind(endpoint);
            dealer.connect(endpoint);
            SampleSupport.waitConnected(routerMonitor, dealerMonitor);

            try (Message request = Message.from(SampleSupport.DEALER_REQUEST)) {
                dealer.send().message(request).submit();
            }

            try (systems.zlink.contracts.messaging.Received received = new systems.zlink.contracts.messaging.Received()) {
                router.recv(received, systems.zlink.contracts.sockets.RecvFlags.NONE);
                String value = SampleSupport.singleUtf8(received);
                if (!SampleSupport.DEALER_REQUEST.equals(value)) {
                    throw new IllegalStateException("unexpected request: " + value);
                }
                try (Message reply = Message.from(SampleSupport.DEALER_REPLY)) {
                    received.send().message(reply).submit();
                }
            }

            try (systems.zlink.contracts.messaging.Received received = new systems.zlink.contracts.messaging.Received()) {
                dealer.recv(received, systems.zlink.contracts.sockets.RecvFlags.NONE);
                String value = SampleSupport.singleUtf8(received);
                if (!SampleSupport.DEALER_REPLY.equals(value)) {
                    throw new IllegalStateException("unexpected reply: " + value);
                }
                System.out.println("[dealer-router/recv] send: \""
                    + SampleSupport.DEALER_REQUEST + "\" \u2192 recv: \"" + value + "\"");
            }
        }
// --8<-- [end:doc]
    }
}
