namespace Zlink.Framework.UnitTests;

/// <summary>
///     Measures the bytes a window of calls allocates on the current thread, several times,
///     and returns the smallest window total. A cost the code under test pays on every call,
///     or once every few calls (storage growth, pooled array refill), is in every window and
///     so is in the minimum. A one-time runtime allocation (tier-up, a lazy runtime table)
///     lands in one window only and cannot decide a strict comparison.
/// </summary>
internal static class PerCallAllocation
{
    public const int Windows = 8;

    // A region is process-wide: allocations or induced collections on another thread
    // can end it before the measured action returns. Only intact windows count.
    internal static async ValueTask<T> MeasureWindowAsync<T>(
        Func<ValueTask<T>> measure,
        long budgetBytes = 8 * 1024 * 1024,
        Action? beforeWindow = null,
        Action? afterWindow = null
    )
    {
        const int attempts = 4;
        for (var attempt = 0; attempt < attempts; attempt++)
        {
            beforeWindow?.Invoke();
            try
            {
                if (!GC.TryStartNoGCRegion(budgetBytes))
                    throw new InvalidOperationException(
                        "Could not reserve a GC-free measurement window."
                    );
                try
                {
                    var result = await measure();
                    if (
                        System.Runtime.GCSettings.LatencyMode
                        == System.Runtime.GCLatencyMode.NoGCRegion
                    )
                        return result;
                }
                finally
                {
                    if (
                        System.Runtime.GCSettings.LatencyMode
                        == System.Runtime.GCLatencyMode.NoGCRegion
                    )
                        GC.EndNoGCRegion();
                }
            }
            finally
            {
                afterWindow?.Invoke();
            }
        }
        throw new InvalidOperationException(
            $"GC-free measurement window ended during all {attempts} attempts; no valid allocation measurement was obtained."
        );
    }

    public static long MinWindow(
        Action action,
        int callsPerWindow,
        int windows = Windows,
        int warmup = 0
    )
    {
        for (var index = 0; index < warmup; index++)
            action();
        return MinWindow(_ => action(), callsPerWindow, windows);
    }

    /// <summary>
    ///     <paramref name="action" /> receives the call index inside the window.
    ///     <paramref name="beforeWindow" /> and <paramref name="afterWindow" /> run outside the
    ///     measured region with the window number, to create and release per-window state.
    /// </summary>
    public static long MinWindow(
        Action<int> action,
        int callsPerWindow,
        int windows = Windows,
        Action<int>? beforeWindow = null,
        Action<int>? afterWindow = null
    )
    {
        var minimum = long.MaxValue;
        for (var window = 0; window < windows; window++)
        {
            var bytes = MeasureWindowAsync(
                    () =>
                    {
                        var before = GC.GetAllocatedBytesForCurrentThread();
                        for (var index = 0; index < callsPerWindow; index++)
                            action(index);
                        return new ValueTask<long>(GC.GetAllocatedBytesForCurrentThread() - before);
                    },
                    beforeWindow: () => beforeWindow?.Invoke(window),
                    afterWindow: () => afterWindow?.Invoke(window)
                )
                .GetAwaiter()
                .GetResult();
            minimum = Math.Min(minimum, bytes);
        }
        return minimum;
    }

    /// <summary>
    ///     Two actions measured in alternating windows, so that both see the same JIT tiering
    ///     and cache state, for tests that compare one against the other with Equal. Both
    ///     windows run in the same GC-free region.
    /// </summary>
    public static (long First, long Second) MinWindowPair(
        Action<int> first,
        Action<int> second,
        int callsPerWindow,
        int windows = Windows
    )
    {
        var minimumFirst = long.MaxValue;
        var minimumSecond = long.MaxValue;
        for (var window = 0; window < windows; window++)
        {
            var (firstBytes, secondBytes) = MeasureWindowAsync(() =>
                {
                    var before = GC.GetAllocatedBytesForCurrentThread();
                    for (var index = 0; index < callsPerWindow; index++)
                        first(index);
                    var firstBytes = GC.GetAllocatedBytesForCurrentThread() - before;
                    before = GC.GetAllocatedBytesForCurrentThread();
                    for (var index = 0; index < callsPerWindow; index++)
                        second(index);
                    var secondBytes = GC.GetAllocatedBytesForCurrentThread() - before;
                    return new ValueTask<(long, long)>((firstBytes, secondBytes));
                })
                .GetAwaiter()
                .GetResult();
            minimumFirst = Math.Min(minimumFirst, firstBytes);
            minimumSecond = Math.Min(minimumSecond, secondBytes);
        }
        return (minimumFirst, minimumSecond);
    }

    public static async Task<long> MinWindowAsync(
        Func<ValueTask> action,
        int callsPerWindow,
        int windows = Windows
    )
    {
        var minimum = long.MaxValue;
        for (var window = 0; window < windows; window++)
        {
            var bytes = await MeasureWindowAsync(async () =>
            {
                var before = GC.GetAllocatedBytesForCurrentThread();
                for (var index = 0; index < callsPerWindow; index++)
                    await action();
                return GC.GetAllocatedBytesForCurrentThread() - before;
            });
            minimum = Math.Min(minimum, bytes);
        }
        return minimum;
    }

    /// <summary>
    ///     For tests whose calls happen on another thread and are measured one by one there:
    ///     the smallest sum over <paramref name="windows" /> equal runs of consecutive samples.
    ///     The caller leaves out samples that overlapped a GC pause.
    /// </summary>
    public static long MinWindow(IReadOnlyList<long> samples, int windows = Windows)
    {
        var size = samples.Count / windows;
        if (size == 0)
            throw new InvalidOperationException("Too few samples for the requested windows.");
        var minimum = long.MaxValue;
        for (var window = 0; window < windows; window++)
        {
            long total = 0;
            for (var index = 0; index < size; index++)
                total += samples[window * size + index];
            minimum = Math.Min(minimum, total);
        }
        return minimum;
    }
}
