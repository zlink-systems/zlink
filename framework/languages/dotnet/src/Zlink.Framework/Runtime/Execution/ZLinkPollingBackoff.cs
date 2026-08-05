namespace Zlink.Framework.Runtime.Execution;

internal sealed class ZLinkPollingBackoff
{
    private static readonly TimeSpan MinDelay = TimeSpan.FromMilliseconds(1);
    private static readonly TimeSpan MaxDelay = TimeSpan.FromMilliseconds(20);
    private int _misses;

    public void Reset()
    {
        _misses = 0;
    }

    public Task NoDataAsync(CancellationToken cancellationToken)
    {
        var misses = Math.Min(++_misses, 6);
        var delayMs = Math.Min(MinDelay.TotalMilliseconds * (1 << (misses - 1)), MaxDelay.TotalMilliseconds);
        return Task.Delay(TimeSpan.FromMilliseconds(delayMs), cancellationToken);
    }
}
