package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.time.Instant;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.monitoring.ZLinkTopologyState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;

final class ZLinkTopologyRuntimeViewsTest {
    @Test
    void hostRelocationMakesClientServerAndFanoutNotReady() {
        ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
        ChannelRegistration clientServer =
            new ChannelRegistration("orders", ChannelKind.CLIENT_SERVER);
        clientServer.enableServer();
        sockets.registerChannel(clientServer);
        RoutingId serverRid = RoutingId.from("local-server");
        var router = (ZLinkBackendRouterSocket) Proxy.newProxyInstance(
            ZLinkBackendRouterSocket.class.getClassLoader(),
            new Class<?>[] {ZLinkBackendRouterSocket.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("peerWeight")) {
                    return 100;
                }
                throw new UnsupportedOperationException(method.getName());
            });
        sockets.registerServer("orders", serverRid, router);
        sockets.setClientServerServerDescriptor("orders", new ZLinkClientServerServerDescriptor(
            "orders", serverRid, 1, 1, "tcp://127.0.0.1:7001", 100,
            ZLinkFrameworkRuntimeState.SERVING, "default", "local-owner", 1, Instant.EPOCH));
        ChannelRegistration fanout =
            new ChannelRegistration("events", ChannelKind.FANOUT);
        fanout.enableSubscriber();
        sockets.registerChannel(fanout);
        AtomicReference<ZLinkFrameworkRuntimeState> hostState =
            new AtomicReference<>(ZLinkFrameworkRuntimeState.SERVING);
        var clientServerRuntime =
            new ZLinkClientServerRuntimeView(sockets, hostState::get);
        var fanoutRuntime = new ZLinkFanoutRuntimeView(
            sockets, () -> null, () -> null, hostState::get);

        assertTrue(clientServerRuntime.snapshot("orders").isReady());
        assertEquals(1, clientServerRuntime.snapshot("orders").readyTargetCount());

        hostState.set(ZLinkFrameworkRuntimeState.RELOCATING);

        assertFalse(clientServerRuntime.snapshot("orders").isReady());
        assertEquals(1, clientServerRuntime.snapshot("orders").readyTargetCount());
        assertEquals(
            ZLinkTopologyState.STOPPING,
            clientServerRuntime.snapshot("orders").state());
        assertFalse(fanoutRuntime.snapshot("events").isReady());
        assertEquals(
            ZLinkTopologyState.STOPPING,
            fanoutRuntime.snapshot("events").state());
    }

    @Test
    void stoppedAndErrorHostStatesAreProjectedExactly() {
        ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
        ChannelRegistration clientServer =
            new ChannelRegistration("orders", ChannelKind.CLIENT_SERVER);
        clientServer.enableServer();
        sockets.registerChannel(clientServer);
        ChannelRegistration fanout =
            new ChannelRegistration("events", ChannelKind.FANOUT);
        fanout.enableSubscriber();
        sockets.registerChannel(fanout);
        AtomicReference<ZLinkFrameworkRuntimeState> hostState =
            new AtomicReference<>(ZLinkFrameworkRuntimeState.STOPPED);
        var clientServerRuntime =
            new ZLinkClientServerRuntimeView(sockets, hostState::get);
        var fanoutRuntime = new ZLinkFanoutRuntimeView(
            sockets, () -> null, () -> null, hostState::get);

        assertEquals(
            ZLinkTopologyState.STOPPED,
            clientServerRuntime.snapshot("orders").state());
        assertEquals(
            ZLinkTopologyState.STOPPED,
            fanoutRuntime.snapshot("events").state());

        hostState.set(ZLinkFrameworkRuntimeState.ERROR);
        assertEquals(
            ZLinkTopologyState.FAILED,
            clientServerRuntime.snapshot("orders").state());
        assertEquals(
            ZLinkTopologyState.FAILED,
            fanoutRuntime.snapshot("events").state());
    }
}
