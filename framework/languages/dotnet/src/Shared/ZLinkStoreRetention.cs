namespace Zlink.Framework.Internal;

internal static class ZLinkStoreRetention
{
    internal static long ToMilliseconds(TimeSpan retention)
    {
        if (retention <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(retention));

        var ticks = retention.Ticks;
        return ticks / TimeSpan.TicksPerMillisecond
            + (ticks % TimeSpan.TicksPerMillisecond == 0 ? 0 : 1);
    }
}
