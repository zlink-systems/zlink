using Zlink.Framework.Contracts.Configuration;

namespace Zlink.Framework.Runtime.Configuration;

internal sealed class ZLinkInboundDispatchOptionsModel : IZLinkInboundDispatchOptions
{
    private ZLinkCoreHwmProfile _coreHwmProfile = ZLinkCoreHwmProfile.Balanced;
    private ZLinkApplicationJobQueueProfile _applicationJobQueueProfile =
        ZLinkApplicationJobQueueProfile.Balanced;

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
}
