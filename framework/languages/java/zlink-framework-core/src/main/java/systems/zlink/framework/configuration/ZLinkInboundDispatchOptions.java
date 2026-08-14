package systems.zlink.framework.configuration;

import java.util.OptionalLong;

/** Host-wide Core HWM forwarding and Application Job Queue configuration. */
public interface ZLinkInboundDispatchOptions {
    OptionalLong coreHwmMemoryLimitBytes();

    void setCoreHwmMemoryLimitBytes(long value);

    OptionalLong coreHwmBudgetBytes();

    void setCoreHwmBudgetBytes(long value);

    ZLinkCoreHwmProfile coreHwmProfile();

    void setCoreHwmProfile(ZLinkCoreHwmProfile value);

    ZLinkApplicationJobQueueProfile applicationJobQueueProfile();

    void setApplicationJobQueueProfile(ZLinkApplicationJobQueueProfile value);

    OptionalLong maxQueuedApplicationJobs();

    void setMaxQueuedApplicationJobs(long value);
}
