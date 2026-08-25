namespace Zlink.Framework.Contracts.Configuration;

public enum ZLinkCoreHwmProfile
{
    Compact = 0,
    LowLatency = 1,
    Balanced = 2,
    Throughput = 3
}

public enum ZLinkApplicationJobQueueProfile
{
    Compact = 0,
    LowLatency = 1,
    Balanced = 2,
    Throughput = 3
}

public enum ZLinkApplicationJobQueuePressureState
{
    Running = 0,
    Paused = 1
}

/// <summary>
/// Configures Core HWM and the host-wide Application Job Queue independently.
/// </summary>
public interface IZLinkInboundDispatchOptions
{
    ulong? CoreHwmMemoryLimitBytes { get; set; }

    ulong? CoreHwmBudgetBytes { get; set; }

    ZLinkCoreHwmProfile CoreHwmProfile { get; set; }

    ZLinkApplicationJobQueueProfile ApplicationJobQueueProfile { get; set; }

    ulong? MaxQueuedApplicationJobs { get; set; }

    uint ApplicationJobQueuePauseThresholdPercent { get; set; }

    uint ApplicationJobQueueResumeThresholdPercent { get; set; }
}
