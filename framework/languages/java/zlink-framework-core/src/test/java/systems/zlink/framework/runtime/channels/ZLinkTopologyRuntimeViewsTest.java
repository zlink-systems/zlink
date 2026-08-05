package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.monitoring.ZLinkTopologyState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;

final class ZLinkTopologyRuntimeViewsTest {
    @Test
    void hostRelocationMakesClientServerAndFanoutNotReady() {
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
            new AtomicReference<>(ZLinkFrameworkRuntimeState.SERVING);
        var clientServerRuntime =
            new ZLinkClientServerRuntimeView(sockets, hostState::get);
        var fanoutRuntime = new ZLinkFanoutRuntimeView(
            sockets, () -> null, hostState::get);

        assertTrue(clientServerRuntime.snapshot("orders").isReady());

        hostState.set(ZLinkFrameworkRuntimeState.RELOCATING);

        assertFalse(clientServerRuntime.snapshot("orders").isReady());
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
            sockets, () -> null, hostState::get);

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
