package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;

final class ChannelNetworkDefaultsTest {
    @Test
    void automaticFanoutPublisherRequiresExactlyOneIdentityMode() {
        ChannelRegistration missing =
            new ChannelRegistration("missing", ChannelKind.FANOUT);
        ChannelBuilders.fanout(missing, "127.0.0.1", "127.0.0.1")
            .enablePublisher(0);

        ChannelRegistration duplicate =
            new ChannelRegistration("duplicate", ChannelKind.FANOUT);
        ChannelBuilders.fanout(duplicate, "127.0.0.1", "127.0.0.1")
            .enablePublisher(0)
            .setRoutingId(RoutingId.from("publisher-fixed"))
            .setRoutingIdPrefix("publisher-allocated");

        assertThrows(ZLinkConfigurationException.class, () -> missing.validate(true));
        assertThrows(ZLinkConfigurationException.class, () -> duplicate.validate(true));
    }

    @Test
    void manualFanoutPublisherDoesNotRequireDescriptorIdentity() {
        ChannelRegistration registration =
            new ChannelRegistration("manual", ChannelKind.FANOUT);
        ChannelBuilders.fanout(registration, "127.0.0.1", "127.0.0.1")
            .enablePublisher(0);

        assertDoesNotThrow(() -> registration.validate(false));
    }

    @Test
    void applicationFanoutPublishRejectsOnlyTheExactReservedTopic() {
        assertThrows(
            ZLinkConfigurationException.class,
            () -> ZLinkChannelRuntime.requireApplicationFanoutTopic("\u0001ZLF1"));
        assertDoesNotThrow(
            () -> ZLinkChannelRuntime.requireApplicationFanoutTopic("\u0001ZLF1.more"));
    }

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
