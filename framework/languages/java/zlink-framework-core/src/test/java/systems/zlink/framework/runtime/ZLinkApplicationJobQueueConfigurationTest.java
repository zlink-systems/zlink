package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.configuration.ZLinkCoreHwmProfile;
import systems.zlink.framework.configuration.ZLinkFrameworkOptions;
import systems.zlink.framework.configuration.ZLinkInboundDispatchOptions;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

final class ZLinkApplicationJobQueueConfigurationTest {
    @Test
    void exposesTheExactInboundDispatchSurfaceAndIndependentDefaults()
        throws Exception {
        assertEquals(
            ZLinkInboundDispatchOptions.class,
            ZLinkFrameworkOptions.class
                .getMethod("configureInboundDispatch")
                .getReturnType());
        assertEquals(
            List.of("COMPACT", "LOW_LATENCY", "BALANCED", "THROUGHPUT"),
            java.util.Arrays.stream(ZLinkApplicationJobQueueProfile.values())
                .map(Enum::name)
                .toList());
        assertEquals(
            List.of("COMPACT", "LOW_LATENCY", "BALANCED", "THROUGHPUT"),
            java.util.Arrays.stream(ZLinkCoreHwmProfile.values())
                .map(Enum::name)
                .toList());
        assertThrows(
            NoSuchMethodException.class,
            () -> ZLinkFrameworkOptions.class.getMethod("configureCoreHwm"));

        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        ZLinkInboundDispatchOptions inbound = options.configureInboundDispatch();
        assertSame(inbound, options.configureInboundDispatch());
        assertEquals(ZLinkCoreHwmProfile.BALANCED, inbound.coreHwmProfile());
        assertEquals(
            ZLinkApplicationJobQueueProfile.BALANCED,
            inbound.applicationJobQueueProfile());
        assertFalse(inbound.coreHwmMemoryLimitBytes().isPresent());
        assertFalse(inbound.coreHwmBudgetBytes().isPresent());
        assertFalse(inbound.maxQueuedApplicationJobs().isPresent());
        assertEquals(80, inbound.applicationJobQueuePauseThresholdPercent());
        assertEquals(60, inbound.applicationJobQueueResumeThresholdPercent());
    }

    @Test
    void keepsCoreAndJobProfilesIndependentAndValidatesManualRange() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        ZLinkInboundDispatchOptions inbound = options.configureInboundDispatch();
        inbound.setCoreHwmProfile(ZLinkCoreHwmProfile.COMPACT);
        inbound.setApplicationJobQueueProfile(
            ZLinkApplicationJobQueueProfile.THROUGHPUT);
        inbound.setCoreHwmMemoryLimitBytes(8L * 1024 * 1024);
        inbound.setCoreHwmBudgetBytes(4L * 1024 * 1024);
        inbound.setMaxQueuedApplicationJobs(Integer.MAX_VALUE);

        assertEquals(ZLinkCoreHwmProfile.COMPACT, inbound.coreHwmProfile());
        assertEquals(
            ZLinkApplicationJobQueueProfile.THROUGHPUT,
            inbound.applicationJobQueueProfile());
        assertEquals(Integer.MAX_VALUE,
            inbound.maxQueuedApplicationJobs().orElseThrow());
        assertThrows(
            ZLinkConfigurationException.class,
            () -> inbound.setMaxQueuedApplicationJobs(0));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> inbound.setMaxQueuedApplicationJobs(-1));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> inbound.setMaxQueuedApplicationJobs((long) Integer.MAX_VALUE + 1));
    }

    @Test
    void validatesStartupOnlyPauseAndResumeThresholds() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        ZLinkInboundDispatchOptions inbound = options.configureInboundDispatch();
        inbound.setApplicationJobQueuePauseThresholdPercent(90);
        inbound.setApplicationJobQueueResumeThresholdPercent(50);
        assertEquals(90, inbound.applicationJobQueuePauseThresholdPercent());
        assertEquals(50, inbound.applicationJobQueueResumeThresholdPercent());
        assertThrows(
            ZLinkConfigurationException.class,
            () -> inbound.setApplicationJobQueuePauseThresholdPercent(0));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> inbound.setApplicationJobQueueResumeThresholdPercent(100));

        DefaultZLinkFrameworkOptions invalid = new DefaultZLinkFrameworkOptions();
        invalid.configureInboundDispatch()
            .setApplicationJobQueuePauseThresholdPercent(60);
        invalid.configureInboundDispatch()
            .setApplicationJobQueueResumeThresholdPercent(60);
        assertThrows(ZLinkConfigurationException.class, invalid::validate);
    }
}
