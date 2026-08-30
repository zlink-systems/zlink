package systems.zlink.framework.runtime.configuration;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.time.Duration;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

final class ZLinkFrameworkRegistrationTest {
    @Test
    void messageFollowDurationDefaultsToCommonContract() {
        var registration = new ZLinkFrameworkRegistration();

        assertEquals(
            Duration.ofSeconds(30),
            registration.locations().options().messageFollowDuration());
    }

}
