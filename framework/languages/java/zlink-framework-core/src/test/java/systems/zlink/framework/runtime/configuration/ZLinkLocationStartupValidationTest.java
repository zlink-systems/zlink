package systems.zlink.framework.runtime.configuration;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;

import java.time.Duration;

final class ZLinkLocationStartupValidationTest {
    @Test
    void startupRelationRejectsEqualityAndAcceptsOneNanosecondOfHeadroom() {
        for (long interval : new long[] {2, 4}) {
            ZLinkFrameworkRegistration registration = new ZLinkFrameworkRegistration();
            registration.locations().setStoreInstance(new ZLinkInMemoryLocationStore());
            var options = registration.locations().options();
            options.setOwnerLeaseRenewInterval(Duration.ofSeconds(interval));
            options.setOwnerLeaseRenewTimeout(Duration.ofSeconds(3));
            long boundaryTtl = Math.max(interval, 3) + 3 + 5;
            options.setOwnerLeaseTtl(Duration.ofSeconds(boundaryTtl));
            assertThrows(ZLinkConfigurationException.class, registration::validate);
            options.setOwnerLeaseTtl(Duration.ofSeconds(boundaryTtl).minusNanos(1));
            assertThrows(ZLinkConfigurationException.class, registration::validate);
            options.setOwnerLeaseTtl(Duration.ofSeconds(boundaryTtl).plusNanos(1));
            assertDoesNotThrow(registration::validate);
        }
    }

    @Test
    void startupRelationAppliesOnlyWhenLocationStoreIsEnabled() {
        ZLinkFrameworkRegistration registration = new ZLinkFrameworkRegistration();
        registration.locations().setStoreInstance(new ZLinkInMemoryLocationStore());
        assertDoesNotThrow(registration::validate);
        registration.locations().options().setOwnerLeaseTtl(Duration.ofSeconds(1));
        assertThrows(ZLinkConfigurationException.class, registration::validate);
        registration.locations().setStoreInstance(null);
        assertDoesNotThrow(registration::validate);
    }

    @Test
    void startupRelationChecksLargeDurationsWithoutOverflow() {
        ZLinkFrameworkRegistration registration = new ZLinkFrameworkRegistration();
        registration.locations().setStoreInstance(new ZLinkInMemoryLocationStore());
        var options = registration.locations().options();
        options.setOwnerLeaseRenewInterval(Duration.ofSeconds(Long.MAX_VALUE - 6));
        options.setOwnerLeaseTtl(Duration.ofSeconds(Long.MAX_VALUE));
        assertThrows(ZLinkConfigurationException.class, registration::validate);
        options.setOwnerLeaseRenewInterval(Duration.ofSeconds(Long.MAX_VALUE - 9));
        assertDoesNotThrow(registration::validate);
    }
}
