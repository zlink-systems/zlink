package systems.zlink.framework.runtime.mesh;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkConfigurationException;

import java.time.Duration;

final class MeshNodeRegistrationSubmitTimeoutTest {
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
