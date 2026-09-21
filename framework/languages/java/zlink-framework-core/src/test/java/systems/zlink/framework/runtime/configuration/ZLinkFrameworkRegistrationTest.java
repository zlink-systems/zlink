package systems.zlink.framework.runtime.configuration;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import java.time.Duration;

final class ZLinkFrameworkRegistrationTest {
    @Test
    void messageFollowDurationDefaultsToCommonContract() {
        var registration = new ZLinkFrameworkRegistration();

        assertEquals(
                Duration.ofSeconds(30), registration.locations().options().messageFollowDuration());
    }
}
