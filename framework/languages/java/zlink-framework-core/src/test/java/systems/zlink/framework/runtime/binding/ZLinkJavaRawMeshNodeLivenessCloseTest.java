package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.ReceiveFlowState;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkJavaRawMeshNodeLivenessCloseTest {
    private static final Duration OBSERVE_WINDOW = Duration.ofSeconds(2);
    private static final Duration POLL_WAIT = Duration.ofMillis(10);

    @Test
    void expiryRemovesPhysicalTargetAndSettlesPendingSendOnce() throws Exception {
        verify(false, false);
    }

    @Test
    void currentReceiveKeepsPhysicalTarget() throws Exception {
        verify(true, false);
    }

    @Test
    void retiredDeadlineDoesNotCloseReplacementAdmission() throws Exception {
        verify(false, true);
    }

    private static void verify(boolean receive, boolean replacement) throws Exception {
        var local = RoutingId.from("liveness-local");
        var peer = RoutingId.from("liveness-peer");
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "liveness-close");
                var target = context.createDealerSocket()) {
            var port = (ZLinkJavaRawServicePort) field(node, "port");
            context.options().autoHwmEnabled(false);
            var source = port.openRouter(local);
            target.setRoutingId(peer);
            set(node, "router", source);
            final int recordBytes = 65536;
            target.options().recvHwm(recordBytes + 64);
            target.options().receiveFlowState(ReceiveFlowState.PAUSED);
            source.options().sendHwm(recordBytes + 64);
            String endpoint = "inproc://liveness-close-" + System.nanoTime();
            source.bind(endpoint);
            target.connect(endpoint);
            port.ensureReceivePollerRegistered(source);
            long readyBy = System.nanoTime() + OBSERVE_WINDOW.toNanos();
            while (source.routesSnapshot().isEmpty() && System.nanoTime() < readyBy)
                port.waitForReady(source, POLL_WAIT);
            assertEquals(1, source.routesSnapshot().size());
            var topology = new ZLinkServiceTopologyRegistry(descriptor(local));
            set(node, "topology", topology);
            assertEquals(
                    ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                    topology.admit(
                            descriptor(peer), "route:11", ServiceWireConstants.COMMAND_HELLO));
            var liveness = (ZLinkServiceLivenessRegistry) field(node, "liveness");
            var retired = liveness.admit(peer, "route:11", 0);
            topology.bindLiveness(peer, retired);
            CompletionStage<Void> pending = null;
            for (int i = 0; i < 16; i++) {
                var send = port.send(source, peer, List.of(new byte[recordBytes]));
                pending = send;
            }
            assertNotNull(pending, "fixture must create pending admission");
            Thread.sleep(25);
            assertFalse(
                    pending.toCompletableFuture().isDone(),
                    pending.toCompletableFuture().toString());
            var terminals = new AtomicInteger();
            var observed = pending.whenComplete((ignored, failure) -> terminals.incrementAndGet());
            if (receive) retired.recordReceived(Duration.ofSeconds(14).toNanos());
            if (replacement) {
                topology.admit(descriptor(peer), "route:12", ServiceWireConstants.COMMAND_HELLO);
                topology.bindLiveness(
                        peer, liveness.admit(peer, "route:12", Duration.ofSeconds(14).toNanos()));
            }
            tick(node, Duration.ofSeconds(15).toNanos());
            if (receive || replacement) {
                assertTrue(topology.peer(peer).isPresent());
                assertEquals(1, source.routesSnapshot().size());
                assertFalse(pending.toCompletableFuture().isDone());
                source.disconnectRid(peer);
            } else {
                assertTrue(topology.peer(peer).isEmpty());
                tick(node, Duration.ofSeconds(16).toNanos());
            }
            long end = System.nanoTime() + OBSERVE_WINDOW.toNanos();
            while (!observed.toCompletableFuture().isDone() && System.nanoTime() < end)
                port.waitForReady(source, POLL_WAIT);
            assertTrue(observed.toCompletableFuture().isCompletedExceptionally());
            assertEquals(1, terminals.get());
            // disconnectRid settles the waiting send at once, but the selected-route row leaves
            // the snapshot only after the asynchronous pipe termination (Core socket
            // zlink_disconnect_rid, Core ROUTER §10.1). The two have no order between them.
            while (!source.routesSnapshot().isEmpty() && System.nanoTime() < end)
                port.waitForReady(source, POLL_WAIT);
            assertTrue(source.routesSnapshot().isEmpty());
        }
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid) {
        return new ZLinkServiceNodeDescriptor(
                "liveness-close",
                rid,
                1,
                1,
                "inproc://" + rid,
                List.of(),
                ZLinkServiceNodeDescriptor.State.SERVING,
                "test-security",
                1,
                List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                ZLinkServiceNodeDescriptor.ObjectRole.NONE,
                1,
                1,
                0,
                0,
                0);
    }

    private static Object field(Object owner, String name) throws Exception {
        Field field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(owner);
    }

    private static void set(Object owner, String name, Object value) throws Exception {
        Field field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(owner, value);
    }

    private static void tick(ZLinkJavaRawMeshNode node, long now) throws Exception {
        Method tick = ZLinkJavaRawMeshNode.class.getDeclaredMethod("tickLiveness", long.class);
        tick.setAccessible(true);
        tick.invoke(node, now);
    }
}
