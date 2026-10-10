namespace Zlink.Framework.Runtime.Configuration;

internal static class ZLinkFanoutSendTimeout
{
    internal static readonly TimeSpan Default = TimeSpan.FromSeconds(1);

    internal static TimeSpan Normalize(TimeSpan timeout)
    {
        if (timeout <= TimeSpan.Zero)
            throw new ZLinkConfigurationException("Send timeout must be greater than zero.");

        var milliseconds = timeout.Ticks / TimeSpan.TicksPerMillisecond;
        if (timeout.Ticks % TimeSpan.TicksPerMillisecond != 0)
            milliseconds++;
        if (milliseconds > int.MaxValue)
            throw new ZLinkConfigurationException(
                $"Send timeout must not exceed {int.MaxValue} milliseconds."
            );
        return TimeSpan.FromMilliseconds(milliseconds);
    }
}
