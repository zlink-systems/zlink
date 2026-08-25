package systems.zlink.framework.runtime.configuration;

import java.util.Objects;
import java.util.OptionalLong;
import java.util.concurrent.Executor;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.configuration.ZLinkCoreHwmProfile;
import systems.zlink.framework.configuration.ZLinkInboundDispatchOptions;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

/** Startup-only model for the host's single inbound-dispatch policy. */
public final class ZLinkInboundDispatchRegistration
    implements ZLinkInboundDispatchOptions {
    private Long coreHwmMemoryLimitBytes;
    private Long coreHwmBudgetBytes;
    private ZLinkCoreHwmProfile coreHwmProfile = ZLinkCoreHwmProfile.BALANCED;
    private ZLinkApplicationJobQueueProfile applicationJobQueueProfile =
        ZLinkApplicationJobQueueProfile.BALANCED;
    private Long maxQueuedApplicationJobs;
    private int applicationJobQueuePauseThresholdPercent = 80;
    private int applicationJobQueueResumeThresholdPercent = 60;
    private ZLinkApplicationJobQueue applicationJobQueue;

    @Override
    public synchronized OptionalLong coreHwmMemoryLimitBytes() {
        return optional(coreHwmMemoryLimitBytes);
    }

    @Override
    public synchronized void setCoreHwmMemoryLimitBytes(long value) {
        requireMutable();
        coreHwmMemoryLimitBytes = positive(value, "CoreHwmMemoryLimitBytes");
    }

    @Override
    public synchronized OptionalLong coreHwmBudgetBytes() {
        return optional(coreHwmBudgetBytes);
    }

    @Override
    public synchronized void setCoreHwmBudgetBytes(long value) {
        requireMutable();
        coreHwmBudgetBytes = positive(value, "CoreHwmBudgetBytes");
    }

    @Override
    public synchronized ZLinkCoreHwmProfile coreHwmProfile() {
        return coreHwmProfile;
    }

    @Override
    public synchronized void setCoreHwmProfile(ZLinkCoreHwmProfile value) {
        requireMutable();
        coreHwmProfile = Objects.requireNonNull(value, "coreHwmProfile");
    }

    @Override
    public synchronized ZLinkApplicationJobQueueProfile
        applicationJobQueueProfile() {
        return applicationJobQueueProfile;
    }

    @Override
    public synchronized void setApplicationJobQueueProfile(
        ZLinkApplicationJobQueueProfile value) {
        requireMutable();
        applicationJobQueueProfile = Objects.requireNonNull(
            value, "applicationJobQueueProfile");
    }

    @Override
    public synchronized OptionalLong maxQueuedApplicationJobs() {
        return optional(maxQueuedApplicationJobs);
    }

    @Override
    public synchronized void setMaxQueuedApplicationJobs(long value) {
        requireMutable();
        if (value < 1 || value > Integer.MAX_VALUE) {
            throw new ZLinkConfigurationException(
                "MaxQueuedApplicationJobs must be in 1..2147483647");
        }
        maxQueuedApplicationJobs = value;
    }

    @Override
    public synchronized int applicationJobQueuePauseThresholdPercent() {
        return applicationJobQueuePauseThresholdPercent;
    }

    @Override
    public synchronized void setApplicationJobQueuePauseThresholdPercent(
        int value) {
        requireMutable();
        if (value < 1 || value > 100) {
            throw new ZLinkConfigurationException(
                "ApplicationJobQueuePauseThresholdPercent must be in 1..100");
        }
        applicationJobQueuePauseThresholdPercent = value;
    }

    @Override
    public synchronized int applicationJobQueueResumeThresholdPercent() {
        return applicationJobQueueResumeThresholdPercent;
    }

    @Override
    public synchronized void setApplicationJobQueueResumeThresholdPercent(
        int value) {
        requireMutable();
        if (value < 0 || value > 99) {
            throw new ZLinkConfigurationException(
                "ApplicationJobQueueResumeThresholdPercent must be in 0..99");
        }
        applicationJobQueueResumeThresholdPercent = value;
    }

    public synchronized ZLinkApplicationJobQueue applicationJobQueue(
        Executor handlerExecutor) {
        if (applicationJobQueue == null) {
            if (applicationJobQueueResumeThresholdPercent
                >= applicationJobQueuePauseThresholdPercent) {
                throw new ZLinkConfigurationException(
                    "ApplicationJobQueueResumeThresholdPercent must be less than ApplicationJobQueuePauseThresholdPercent");
            }
            applicationJobQueue = new ZLinkApplicationJobQueue(
                applicationJobQueueProfile,
                optional(maxQueuedApplicationJobs),
                ZLinkApplicationJobQueue.productionProcessorCandidates(
                    handlerExecutor),
                applicationJobQueuePauseThresholdPercent,
                applicationJobQueueResumeThresholdPercent);
        }
        return applicationJobQueue;
    }

    private void requireMutable() {
        if (applicationJobQueue != null) {
            throw new ZLinkConfigurationException(
                "Inbound dispatch options are fixed at startup");
        }
    }

    private static OptionalLong optional(Long value) {
        return value == null ? OptionalLong.empty() : OptionalLong.of(value);
    }

    private static long positive(long value, String name) {
        if (value <= 0) {
            throw new ZLinkConfigurationException(name + " must be positive");
        }
        return value;
    }
}
