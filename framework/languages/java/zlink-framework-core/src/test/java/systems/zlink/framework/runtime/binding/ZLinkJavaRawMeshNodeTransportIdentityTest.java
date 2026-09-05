package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Method;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.MonitorEventType;

final class ZLinkJavaRawMeshNodeTransportIdentityTest {
    private static final long INTENT = 2;
    private static final RoutingId PEER = RoutingId.from("transport-identity-peer");
    private static final String ENDPOINT = "inproc://transport-identity-peer";

    @Test
    void foreignTerminationPreservesIntentUntilItsRecordedTransportDisconnects()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            installIntent(node);
            ready(node, 1436, 0);
            assertTrue(node.hasLivePeerIntent(ENDPOINT));

            // The observed completion-lane termination, then each mismatch
            // independently, must preserve the admitted application transport.
            for (var event : new MonitorEvent[] {
                    event(MonitorEventType.DISCONNECTED, 1439, 1),
                    event(MonitorEventType.DISCONNECTED, 1436, 1),
                    event(MonitorEventType.DISCONNECTED, 1439, 0),
                    event(MonitorEventType.DISCONNECTED, 0, 0)}) {
                terminate(node, event);
                assertTrue(node.hasLivePeerIntent(ENDPOINT), event.toString());
                assertFalse(isClosed(node), event.toString());
            }

            terminate(node, event(MonitorEventType.DISCONNECTED, 1436, 0));
            assertFalse(node.hasLivePeerIntent(ENDPOINT));
            assertTrue(isClosed(node));
        }
    }

    @Test
    void intentClosesOnlyAfterEveryRecordedTransportTerminates()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            installIntent(node);
            ready(node, 1436, 0);
            ready(node, 1439, 1);

            terminate(node, event(MonitorEventType.DISCONNECTED, 1439, 1));
            assertTrue(node.hasLivePeerIntent(ENDPOINT));
            assertFalse(isClosed(node));
            terminate(node, event(MonitorEventType.DISCONNECTED, 1439, 1));
            assertTrue(node.hasLivePeerIntent(ENDPOINT));
            assertFalse(isClosed(node));

            terminate(node, event(MonitorEventType.CLOSED, 1436, 0));
            assertFalse(node.hasLivePeerIntent(ENDPOINT));
            assertTrue(isClosed(node));
        }
    }

    @Test
    void closeRequestDoesNotAttributeAnUnrecordedTerminationToIntent()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            installIntent(node);
            requestClose(node);

            terminate(node, event(MonitorEventType.DISCONNECTED, 1439, 1));
            assertFalse(isClosed(node));
            assertTrue(node.isPeerConnectionClosing(INTENT));
        }
    }

    @Test
    void readyObservedAfterCloseRequestStillOwnsItsTermination()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            installIntent(node);
            requestClose(node);
            ready(node, 1436, 0);
            assertFalse(node.hasLivePeerIntent(ENDPOINT));
            assertTrue(node.isPeerConnectionClosing(INTENT));

            terminate(node, event(MonitorEventType.DISCONNECTED, 1439, 1));
            assertFalse(isClosed(node));
            assertTrue(node.isPeerConnectionClosing(INTENT));

            terminate(node, event(MonitorEventType.DISCONNECTED, 1436, 0));
            assertTrue(isClosed(node));
            assertFalse(node.isPeerConnectionClosing(INTENT));
        }
    }

    private static void requestClose(ZLinkJavaRawMeshNode node) throws Exception {
        var closingField = ZLinkJavaRawMeshNode.class
            .getDeclaredField("closeRequestedPeerIntents");
        closingField.setAccessible(true);
        @SuppressWarnings("unchecked")
        var closing = (Set<Long>) closingField.get(node);
        closing.add(INTENT);
    }

    private static void installIntent(ZLinkJavaRawMeshNode node)
        throws Exception {
        // Seed only the configured intent; drive the real monitor handlers
        // synchronously so native scheduling cannot hide foreign attribution.
        var intentClass = Class.forName(
            ZLinkJavaRawMeshNode.class.getName() + "$PeerIntent");
        var constructor = intentClass.getDeclaredConstructor(
            String.class, RoutingId.class, long.class, String.class, long.class);
        constructor.setAccessible(true);
        var intentsField = ZLinkJavaRawMeshNode.class.getDeclaredField("peerIntents");
        intentsField.setAccessible(true);
        @SuppressWarnings("unchecked")
        var intents = (Map<Long, Object>) intentsField.get(node);
        intents.put(INTENT, constructor.newInstance(ENDPOINT, PEER, 0L, null, 0L));
    }

    private static MonitorEvent event(
        MonitorEventType type, long connectionId, int lane) {
        return new MonitorEvent(
            type, type == MonitorEventType.CONNECTION_READY ? 1 : 4,
            Optional.of(PEER), "inproc://transport-identity-local", ENDPOINT,
            connectionId, lane, type == MonitorEventType.CONNECTION_READY ? 1 : 0);
    }

    private static void ready(ZLinkJavaRawMeshNode node, long id, int lane)
        throws Exception {
        Method active = ZLinkJavaRawMeshNode.class.getDeclaredMethod(
            "markPeerIntentsActive", MonitorEvent.class, RoutingId.class);
        active.setAccessible(true);
        active.invoke(node, event(MonitorEventType.CONNECTION_READY, id, lane), PEER);
    }

    private static void terminate(ZLinkJavaRawMeshNode node, MonitorEvent event)
        throws Exception {
        Method terminal = ZLinkJavaRawMeshNode.class.getDeclaredMethod(
            "markPeerIntentsClosed", MonitorEvent.class);
        terminal.setAccessible(true);
        terminal.invoke(node, event);
    }

    private static boolean isClosed(ZLinkJavaRawMeshNode node) throws Exception {
        Method closed = ZLinkJavaRawMeshNode.class.getDeclaredMethod(
            "peerIntentIsClosed", long.class);
        closed.setAccessible(true);
        return (boolean) closed.invoke(node, INTENT);
    }
}
