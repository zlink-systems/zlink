package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import java.lang.reflect.Modifier;
import java.lang.reflect.Method;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class MonitorBehaviorContractTest {
    @Test
    public void blockingRecvReturnsObservedLifecycleEvent() throws Exception {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             PairSocket server = ctx.createPairSocket();
             PairSocket client = ctx.createPairSocket();
             var serverMonitor = server.monitorOpen(
               MonitorEventType.LISTENING, MonitorEventType.ACCEPTED,
               MonitorEventType.CONNECTED)) {
            CompletableFuture<MonitorEvent> eventFuture =
                CompletableFuture.supplyAsync(serverMonitor::recv);

            String endpoint = TestSupport.tcpEndpoint();
            server.bind(endpoint);
            client.connect(endpoint);
            try (Message payload = Message.from("monitor")) {
                client.send().message(payload).submit();
            }

            MonitorEvent event = eventFuture.get(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS);
            assertTrue(event.event() == MonitorEventType.LISTENING
                || event.event() == MonitorEventType.ACCEPTED
                || event.event() == MonitorEventType.CONNECTED);
        }
    }

    @Test
    public void monitorSocketExposesDocumentedRecvSurface() {
        assertTrue(hasPublicMethod(systems.zlink.contracts.eventing.SocketMonitor.class, "recv"));
        assertTrue(hasPublicMethod(systems.zlink.contracts.eventing.SocketMonitor.class, "recv",
            systems.zlink.contracts.sockets.RecvFlags.class));
        assertTrue(Modifier.isPublic(SocketMonitor.class.getModifiers()));
        assertNotNull(SocketMonitor.IGNORE_HANDLER);
    }

    private static boolean hasPublicMethod(Class<?> type, String name,
                                           Class<?>... parameterTypes) {
        try {
            Method method = type.getMethod(name, parameterTypes);
            return method != null;
        } catch (NoSuchMethodException ex) {
            return false;
        }
    }
}
