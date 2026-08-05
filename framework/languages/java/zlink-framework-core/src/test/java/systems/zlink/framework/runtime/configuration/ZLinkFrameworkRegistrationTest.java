package systems.zlink.framework.runtime.configuration;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Duration;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

final class ZLinkFrameworkRegistrationTest {
    @Test
    void messageFollowDurationDefaultsToCommonContract() {
        var registration = new ZLinkFrameworkRegistration();

        assertEquals(Duration.ofSeconds(30), registration.messageFollowDuration());
    }

    @Test
    void applicationHwmRequiresFiniteListenerMessageLimit() {
        var options = new DefaultZLinkFrameworkOptions();
        var mesh = options.addRouteMesh("orders").listen(0);
        options.configureInboundDispatch().setApplicationHwmBytes(4096);
        mesh.configureRouterSocket().setMaxMessageSize(0);

        assertThrows(ZLinkConfigurationException.class, options::validate);

        options.configureInboundDispatch().setApplicationHwmBytes(0);
        assertDoesNotThrow(options::validate);
    }
}
