package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

final class ChannelNetworkDefaultsTest {
    @Test
    void fanoutPublisherUsesRootHostsUntilOverridden() {
        ChannelRegistration registration =
            new ChannelRegistration("events", ChannelKind.FANOUT);

        ChannelBuilders.fanout(
                registration, "0.0.0.0", "events.example.test")
            .enablePublisher(0);

        assertEquals(
            java.util.List.of("tcp://0.0.0.0:0"),
            registration.publisherBinds());
        assertEquals(
            "events.example.test",
            registration.fanoutAdvertiseHost());
    }

    @Test
    void clientServerUsesRootHostsUntilOverridden() {
        ChannelRegistration registration =
            new ChannelRegistration("orders", ChannelKind.CLIENT_SERVER);

        ChannelBuilders.clientServer(
                registration, "0.0.0.0", "orders.example.test")
            .server()
            .listen(0);

        assertEquals(
            java.util.List.of("tcp://0.0.0.0:0"),
            registration.serverBinds());
        assertEquals(
            "orders.example.test",
            registration.clientServerAdvertiseHost());
    }
}
