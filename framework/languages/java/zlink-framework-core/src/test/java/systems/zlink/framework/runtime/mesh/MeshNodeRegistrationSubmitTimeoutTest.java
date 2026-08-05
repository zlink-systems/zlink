package systems.zlink.framework.runtime.mesh;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Duration;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.errors.ZLinkConfigurationException;

final class MeshNodeRegistrationSubmitTimeoutTest {
    @Test
    void routerAndPublisherRejectNonPositiveOrOverflowingSendTimeouts() {
        MeshNodeRegistration registration = new MeshNodeRegistration("mesh");

        assertThrows(ZLinkConfigurationException.class,
            () -> registration.configureRouterSocket().setSendTimeout(Duration.ZERO));
        assertThrows(ZLinkConfigurationException.class,
            () -> registration.configureSpotPublisher().setSendTimeout(
                Duration.ofMillis(-1)));
        assertThrows(ZLinkConfigurationException.class,
            () -> registration.configureRouterSocket().setSendTimeout(
                Duration.ofDays(365)));
        assertThrows(ZLinkConfigurationException.class,
            () -> registration.configureRouterSocket().setSendTimeout(
                Duration.ofMillis((long) Integer.MAX_VALUE + 1L)));
    }

    @Test
    void positiveSubMillisecondSendTimeoutRemainsConfigured() {
        MeshNodeRegistration registration = new MeshNodeRegistration("mesh");
        Duration value = Duration.ofNanos(1);

        registration.configureRouterSocket().setSendTimeout(value);
        registration.configureSpotPublisher().setSendTimeout(value);

        assertEquals(value,
            registration.configureRouterSocket().sendTimeout().orElseThrow());
        assertEquals(value,
            registration.configureSpotPublisher().sendTimeout().orElseThrow());

        registration.configureRouterSocket().setSendTimeout(null);
        registration.configureSpotPublisher().setSendTimeout(null);
        assertEquals(java.util.Optional.empty(),
            registration.configureRouterSocket().sendTimeout());
        assertEquals(java.util.Optional.empty(),
            registration.configureSpotPublisher().sendTimeout());
    }

    @Test
    void maximumMillisecondSendTimeoutRemainsConfigured() {
        MeshNodeRegistration registration = new MeshNodeRegistration("mesh");
        Duration maximum = Duration.ofMillis(Integer.MAX_VALUE);

        registration.configureRouterSocket().setSendTimeout(maximum);
        registration.configureSpotPublisher().setSendTimeout(maximum);

        assertEquals(maximum,
            registration.configureRouterSocket().sendTimeout().orElseThrow());
        assertEquals(maximum,
            registration.configureSpotPublisher().sendTimeout().orElseThrow());
    }

    @Test
    void instanceSpotIdleTimeoutUsesZeroDefaultAndRejectsNegativeValues() {
        MeshNodeRegistration registration = new MeshNodeRegistration("mesh");

        assertEquals(Duration.ZERO, registration.instanceSpotIdleTimeout());
        Duration configured = Duration.ofMillis(250);
        registration.setInstanceSpotIdleTimeout(configured);
        assertEquals(configured, registration.instanceSpotIdleTimeout());
        assertThrows(ZLinkConfigurationException.class,
            () -> registration.setInstanceSpotIdleTimeout(Duration.ofNanos(-1)));
        assertThrows(ZLinkConfigurationException.class,
            () -> registration.setInstanceSpotIdleTimeout(null));
    }
}
