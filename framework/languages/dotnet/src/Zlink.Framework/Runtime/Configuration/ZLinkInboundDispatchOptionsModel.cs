using Zlink.Framework.Contracts.Configuration;

namespace Zlink.Framework.Runtime.Configuration;

internal sealed class ZLinkInboundDispatchOptionsModel : IZLinkInboundDispatchOptions
{
    private const uint DefaultPauseThresholdPercent = 80;
    private const uint DefaultResumeThresholdPercent = 60;
    private ZLinkCoreHwmProfile _coreHwmProfile = ZLinkCoreHwmProfile.Balanced;
    private ZLinkApplicationJobQueueProfile _applicationJobQueueProfile =
        ZLinkApplicationJobQueueProfile.Balanced;
    private uint _applicationJobQueuePauseThresholdPercent =
        DefaultPauseThresholdPercent;
    private uint _applicationJobQueueResumeThresholdPercent =
        DefaultResumeThresholdPercent;

    public ZLinkCoreHwmProfile CoreHwmProfile
    {
        get => _coreHwmProfile;
        set
        {
            if (!Enum.IsDefined(value))
                throw new ZLinkConfigurationException(
                    $"Unknown CoreHwmProfile value '{(int)value}'.");
            _coreHwmProfile = value;
        }
    }

    public ulong? CoreHwmMemoryLimitBytes { get; set; }

    public ulong? CoreHwmBudgetBytes { get; set; }

    public ZLinkApplicationJobQueueProfile ApplicationJobQueueProfile
    {
        get => _applicationJobQueueProfile;
        set
        {
            if (!Enum.IsDefined(value))
                throw new ZLinkConfigurationException(
                    $"Unknown ApplicationJobQueueProfile value '{(int)value}'.");
            _applicationJobQueueProfile = value;
        }
    }

    public ulong? MaxQueuedApplicationJobs { get; set; }

    public uint ApplicationJobQueuePauseThresholdPercent
    {
        get => _applicationJobQueuePauseThresholdPercent;
        set
        {
            if (value is 0 or > 100)
                throw new ZLinkConfigurationException(
                    "ApplicationJobQueuePauseThresholdPercent must be between 1 and 100.");
            _applicationJobQueuePauseThresholdPercent = value;
        }
    }

    public uint ApplicationJobQueueResumeThresholdPercent
    {
        get => _applicationJobQueueResumeThresholdPercent;
        set
        {
            if (value > 99)
                throw new ZLinkConfigurationException(
                    "ApplicationJobQueueResumeThresholdPercent must be between 0 and 99.");
            _applicationJobQueueResumeThresholdPercent = value;
        }
    }
}
