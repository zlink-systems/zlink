namespace Zlink.Framework.Runtime.Configuration;

internal sealed class ZLinkInboundDispatchOptionsModel : IZLinkInboundDispatchOptions
{
    private ZLinkApplicationHwmProfile _applicationHwmProfile =
        ZLinkApplicationHwmProfile.Balanced;
    private ulong? _processMemoryLimitBytes;

    public ulong? ApplicationHwmBytes { get; set; }

    public ZLinkApplicationHwmProfile ApplicationHwmProfile
    {
        get => _applicationHwmProfile;
        set
        {
            if (!Enum.IsDefined(value))
                throw new ZLinkConfigurationException(
                    $"Unknown ApplicationHwmProfile value '{(int)value}'.");
            _applicationHwmProfile = value;
        }
    }

    public ulong? ProcessMemoryLimitBytes
    {
        get => _processMemoryLimitBytes;
        set
        {
            if (value == 0)
                throw new ZLinkConfigurationException(
                    "ProcessMemoryLimitBytes must be a positive finite byte value.");
            _processMemoryLimitBytes = value;
        }
    }

    internal ulong EffectiveApplicationHwmBytes { get; set; }
}
