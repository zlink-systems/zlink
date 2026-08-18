package systems.zlink.framework.runtime.channels;
import java.util.List;

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
            List.of("tcp://0.0.0.0:0"),
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
            List.of("tcp://0.0.0.0:0"),
            registration.serverBinds());
        assertEquals(
            "orders.example.test",
            registration.clientServerAdvertiseHost());
    }

    @Test
    void bindHostIsBracketedIpv6SafelyBeforeNormalization() {
        // Endpoint notation policy §2.4/§2.5: ChannelBuilders used to
        // concatenate "tcp://" + bindHost + ":" + port directly, which for
        // an IPv6 bind host (unbracketed, multiple colons) fed a shape the
        // shared normalizer cannot safely re-split into host and port.
        ChannelRegistration registration =
            new ChannelRegistration("ipv6", ChannelKind.CLIENT_SERVER);

        ChannelBuilders.clientServer(registration, "::1", null)
            .server()
            .listen(8080);

        assertEquals(List.of("tcp://[::1]:8080"), registration.serverBinds());
    }

    @Test
    void applicationSuppliedEndpointsAreNormalizedAtTheAcceptancePoint() {
        // Endpoint notation policy §2.3: ChannelRegistration.
        // requireEndpointValue is the single chokepoint for every
        // application-supplied bind/manual-connect endpoint.
        ChannelRegistration registration =
            new ChannelRegistration("normalize", ChannelKind.ROUTE_MESH);

        ChannelBuilders.routeMesh(registration).enableServer("TCP://Host:0080/");
        assertEquals(List.of("tcp://host:80"), registration.routeBinds());

        var routeMesh = ChannelBuilders.routeMesh(registration);
        routeMesh.enableClient("TCP://[FE80::1%Eth0]:0080");
        assertEquals(
            List.of("tcp://[fe80::1%Eth0]:80"),
            registration.routeManualEndpoints());

        // Disconnect must normalize its argument the same way connect did,
        // so an equivalent-but-differently-notated endpoint still matches.
        // The zone id's case is preserved verbatim by policy (§2.2), so it
        // must match exactly -- only scheme/host case and the port's
        // leading zero differ here.
        routeMesh.clientConnections().disconnect("tcp://[fe80::1%Eth0]:080");
        assertEquals(List.of(), registration.routeManualEndpoints());
    }
}
