package systems.zlink.framework.runtime.streams;
import java.time.Duration;
import systems.zlink.framework.configuration.ZLinkStreamNodeBuilder;
import systems.zlink.framework.streams.ZLinkSession;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
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
    void sessionRelocationSealTimeoutIsNotAStreamNetworkOption() {
        assertThrows(NoSuchMethodException.class, () ->
            ZLinkStreamNodeBuilder.class.getMethod(
                "sessionRelocationSealTimeout", Duration.class));
        assertThrows(NoSuchMethodException.class, () ->
            StreamNodeRegistration.class.getMethod(
                "sessionRelocationSealTimeout"));
    }

    @Test
    void streamSocketLimitUsesSocketConfiguration() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        var stream = options.addStreamNode("gateway");
        stream.bind("inproc://gateway");
        stream.registerSession(ZLinkSession.class);
        stream.configureSocket().setMaxMessageSize(0);

        assertDoesNotThrow(options::validate);
    }

}
