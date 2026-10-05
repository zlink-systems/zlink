package systems.zlink.framework.runtime.mesh;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkConfigurationException;

import java.time.Duration;
import java.util.Optional;

final class MeshNodeRegistrationSubmitTimeoutTest {
    @Test
    void classicPublisherRejectsNonPositiveOrOverflowingSendTimeouts() {
        MeshNodeRegistration registration = new MeshNodeRegistration("mesh");
        for (Duration timeout :
                new Duration[] {
                    Duration.ZERO,
                    Duration.ofMillis(-1),
                    Duration.ofMillis((long) Integer.MAX_VALUE + 1L)
                }) {
            assertThrows(
                    ZLinkConfigurationException.class,
                    () -> registration.configureSpotPublisher().setSendTimeout(timeout));
        }
    }

    @Test
    void positiveSubMillisecondSendTimeoutRemainsConfigured() {
        MeshNodeRegistration registration = new MeshNodeRegistration("mesh");
        Duration value = Duration.ofNanos(1);

        registration.configureSpotPublisher().setSendTimeout(value);

        assertEquals(value, registration.configureSpotPublisher().sendTimeout().orElseThrow());

        registration.configureSpotPublisher().setSendTimeout(null);
        assertEquals(Optional.empty(), registration.configureSpotPublisher().sendTimeout());
    }

    @Test
    void maximumMillisecondSendTimeoutRemainsConfigured() {
        MeshNodeRegistration registration = new MeshNodeRegistration("mesh");
        Duration maximum = Duration.ofMillis(Integer.MAX_VALUE);

        registration.configureSpotPublisher().setSendTimeout(maximum);

        assertEquals(maximum, registration.configureSpotPublisher().sendTimeout().orElseThrow());
    }

    @Test
    void instanceSpotIdleTimeoutUsesZeroDefaultAndRejectsNegativeValues() {
        MeshNodeRegistration registration = new MeshNodeRegistration("mesh");

        assertEquals(Duration.ZERO, registration.instanceSpotIdleTimeout());
        Duration configured = Duration.ofMillis(250);
        registration.setInstanceSpotIdleTimeout(configured);
        assertEquals(configured, registration.instanceSpotIdleTimeout());
        assertThrows(
                ZLinkConfigurationException.class,
                () -> registration.setInstanceSpotIdleTimeout(Duration.ofNanos(-1)));
        assertThrows(
                ZLinkConfigurationException.class,
                () -> registration.setInstanceSpotIdleTimeout(null));
    }
}
