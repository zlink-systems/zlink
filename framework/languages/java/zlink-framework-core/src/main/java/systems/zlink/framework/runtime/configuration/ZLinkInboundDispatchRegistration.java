package systems.zlink.framework.runtime.configuration;

import java.util.Objects;
import java.util.OptionalLong;
import java.util.concurrent.CompletionException;
import java.util.concurrent.Executor;
import java.util.function.Supplier;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.configuration.ZLinkCoreHwmProfile;
import systems.zlink.framework.configuration.ZLinkInboundDispatchOptions;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/** Startup-only model for the host's single inbound-dispatch policy. */
public final class ZLinkInboundDispatchRegistration
    implements ZLinkInboundDispatchOptions {
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private Long coreHwmMemoryLimitBytes;
    private Long coreHwmBudgetBytes;
    private ZLinkCoreHwmProfile coreHwmProfile = ZLinkCoreHwmProfile.BALANCED;
    private ZLinkApplicationJobQueueProfile applicationJobQueueProfile =
        ZLinkApplicationJobQueueProfile.BALANCED;
    private Long maxQueuedApplicationJobs;
    private int applicationJobQueuePauseThresholdPercent = 80;
    private int applicationJobQueueResumeThresholdPercent = 60;
    private ZLinkApplicationJobQueue applicationJobQueue;

    private <T> T inStateLane(Supplier<T> work) {
        try {
            return stateLane.runAsync(work).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) {
                throw runtimeFailure;
            }
            if (cause instanceof Error error) {
                throw error;
            }
            throw failure;
        }
    }

    @Override
    public OptionalLong coreHwmMemoryLimitBytes() {
        return inStateLane(this::coreHwmMemoryLimitBytesCore);
    }

    private OptionalLong coreHwmMemoryLimitBytesCore() {
        return optional(coreHwmMemoryLimitBytes);
    }

    @Override
    public void setCoreHwmMemoryLimitBytes(long value) {
        inStateLane(() -> {
            setCoreHwmMemoryLimitBytesCore(value);
            return null;
        });
    }

    private void setCoreHwmMemoryLimitBytesCore(long value) {
        requireMutable();
        coreHwmMemoryLimitBytes = positive(value, "CoreHwmMemoryLimitBytes");
    }

    @Override
    public OptionalLong coreHwmBudgetBytes() {
        return inStateLane(this::coreHwmBudgetBytesCore);
    }

    private OptionalLong coreHwmBudgetBytesCore() {
        return optional(coreHwmBudgetBytes);
    }

    @Override
    public void setCoreHwmBudgetBytes(long value) {
        inStateLane(() -> {
            setCoreHwmBudgetBytesCore(value);
            return null;
        });
    }

    private void setCoreHwmBudgetBytesCore(long value) {
        requireMutable();
        coreHwmBudgetBytes = positive(value, "CoreHwmBudgetBytes");
    }

    @Override
    public ZLinkCoreHwmProfile coreHwmProfile() {
        return inStateLane(this::coreHwmProfileCore);
    }

    private ZLinkCoreHwmProfile coreHwmProfileCore() {
        return coreHwmProfile;
    }

    @Override
    public void setCoreHwmProfile(ZLinkCoreHwmProfile value) {
        inStateLane(() -> {
            setCoreHwmProfileCore(value);
            return null;
        });
    }

    private void setCoreHwmProfileCore(ZLinkCoreHwmProfile value) {
        requireMutable();
        coreHwmProfile = Objects.requireNonNull(value, "coreHwmProfile");
    }

    @Override
    public ZLinkApplicationJobQueueProfile
        applicationJobQueueProfile() {
        return inStateLane(this::applicationJobQueueProfileCore);
    }

    private ZLinkApplicationJobQueueProfile applicationJobQueueProfileCore() {
        return applicationJobQueueProfile;
    }

    @Override
    public void setApplicationJobQueueProfile(
        ZLinkApplicationJobQueueProfile value) {
        inStateLane(() -> {
            setApplicationJobQueueProfileCore(value);
            return null;
        });
    }

    private void setApplicationJobQueueProfileCore(
        ZLinkApplicationJobQueueProfile value) {
        requireMutable();
        applicationJobQueueProfile = Objects.requireNonNull(
            value, "applicationJobQueueProfile");
    }

    @Override
    public OptionalLong maxQueuedApplicationJobs() {
        return inStateLane(this::maxQueuedApplicationJobsCore);
    }

    private OptionalLong maxQueuedApplicationJobsCore() {
        return optional(maxQueuedApplicationJobs);
    }

    @Override
    public void setMaxQueuedApplicationJobs(long value) {
        inStateLane(() -> {
            setMaxQueuedApplicationJobsCore(value);
            return null;
        });
    }

    private void setMaxQueuedApplicationJobsCore(long value) {
        requireMutable();
        if (value < 1 || value > Integer.MAX_VALUE) {
            throw new ZLinkConfigurationException(
                "MaxQueuedApplicationJobs must be in 1..2147483647");
        }
        maxQueuedApplicationJobs = value;
    }

    @Override
    public int applicationJobQueuePauseThresholdPercent() {
        return inStateLane(this::applicationJobQueuePauseThresholdPercentCore);
    }

    private int applicationJobQueuePauseThresholdPercentCore() {
        return applicationJobQueuePauseThresholdPercent;
    }

    @Override
    public void setApplicationJobQueuePauseThresholdPercent(
        int value) {
        inStateLane(() -> {
            setApplicationJobQueuePauseThresholdPercentCore(value);
            return null;
        });
    }

    private void setApplicationJobQueuePauseThresholdPercentCore(
        int value) {
        requireMutable();
        if (value < 1 || value > 100) {
            throw new ZLinkConfigurationException(
                "ApplicationJobQueuePauseThresholdPercent must be in 1..100");
        }
        applicationJobQueuePauseThresholdPercent = value;
    }

    @Override
    public int applicationJobQueueResumeThresholdPercent() {
        return inStateLane(this::applicationJobQueueResumeThresholdPercentCore);
    }

    private int applicationJobQueueResumeThresholdPercentCore() {
        return applicationJobQueueResumeThresholdPercent;
    }

    @Override
    public void setApplicationJobQueueResumeThresholdPercent(
        int value) {
        inStateLane(() -> {
            setApplicationJobQueueResumeThresholdPercentCore(value);
            return null;
        });
    }

    private void setApplicationJobQueueResumeThresholdPercentCore(
        int value) {
        requireMutable();
        if (value < 0 || value > 99) {
            throw new ZLinkConfigurationException(
                "ApplicationJobQueueResumeThresholdPercent must be in 0..99");
        }
        applicationJobQueueResumeThresholdPercent = value;
    }

    public ZLinkApplicationJobQueue applicationJobQueue(
        Executor handlerExecutor) {
        return inStateLane(() -> applicationJobQueueCore(handlerExecutor));
    }

    private ZLinkApplicationJobQueue applicationJobQueueCore(
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
