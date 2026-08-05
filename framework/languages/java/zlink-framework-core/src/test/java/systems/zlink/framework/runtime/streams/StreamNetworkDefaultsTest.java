package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

final class StreamNetworkDefaultsTest {
    @Test
    void streamListenerUsesRootHostsUntilOverridden() {
        StreamNodeRegistration registration = new StreamNodeRegistration("gateway");

        StreamBuilders.streamNode(
                registration, "0.0.0.0", "stream.example.test")
            .bind(0);

        assertEquals("tcp://0.0.0.0:0", registration.bindEndpoint());
        assertEquals(
            "tcp://stream.example.test:43130",
            registration.advertisedEndpoint("tcp://0.0.0.0:43130"));
    }

    @Test
    void streamSocketUsesFiniteDefaultAndRejectsNegativeLimit() {
        StreamNodeRegistration registration = new StreamNodeRegistration("gateway");

        assertEquals(64L * 1024L, registration.socketConfig().maxMessageSize());
        assertThrows(ZLinkConfigurationException.class,
            () -> registration.socketConfig().setMaxMessageSize(-1));
    }

    @Test
    void streamSocketLimitIsRequiredWhenApplicationHwmIsEnabled() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        var stream = options.addStreamNode("gateway");
        stream.bind("inproc://gateway");
        stream.registerSession(systems.zlink.framework.streams.ZLinkSession.class);
        stream.configureSocket().setMaxMessageSize(0);
        options.configureInboundDispatch().setApplicationHwmBytes(1024);

        assertThrows(ZLinkConfigurationException.class, options::validate);
    }

}
