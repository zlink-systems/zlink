package systems.zlink.samples;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.messaging.TopicMessage;
public final class PubSubRecvSample {
    public static void main(String[] args) {
// --8<-- [start:doc]
        SampleSupport.ensureNative();
        String endpoint = SampleSupport.tcpEndpoint();
        String published = SampleSupport.PUBSUB_TOPIC + "/" + SampleSupport.PUBSUB_PAYLOAD;

        try (Context ctx = Zlink.createContext();
             PubSocket pub = ctx.createPubSocket();
             SubSocket sub = ctx.createSubSocket();
             var pubMonitor = pub.monitorOpen(
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY);
             var subMonitor = sub.monitorOpen(
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY)) {
            pub.bind(endpoint);
            sub.setSubscription(SampleSupport.PUBSUB_TOPIC);
            sub.connect(endpoint);
            SampleSupport.waitPubSubReady(pubMonitor, subMonitor);

            try (Message payload = Message.from(SampleSupport.PUBSUB_PAYLOAD)) {
                pub.publish(SampleSupport.PUBSUB_TOPIC).message(payload).submit();
            }

            try (var received = new TopicMessage()) {
                if (!sub.subscribe(received, RecvFlags.NONE)) {
                    throw new IllegalStateException("no pubsub delivery");
                }
                String value = received.topic() + "/"
                    + received.singlePartOrThrow().toUtf8String();
                if (!published.equals(value)) {
                    throw new IllegalStateException("unexpected delivery: " + value);
                }
                System.out.println("[pubsub/recv] publish: \"" + published
                    + "\" \u2192 subscribe: \"" + value + "\"");
            }
        }
// --8<-- [end:doc]
    }
}
