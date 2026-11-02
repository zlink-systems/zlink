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

    internal static (int Gen0, int Gen1, int Gen2) CollectionCounts() =>
        (GC.CollectionCount(0), GC.CollectionCount(1), GC.CollectionCount(2));

    internal static bool CollectionCountsUnchanged((int Gen0, int Gen1, int Gen2) before) =>
        before == CollectionCounts();

    private static long RequireMinimum(long minimum) =>
        minimum == long.MaxValue
            ? throw new InvalidOperationException(
                "All allocation measurement windows overlapped a GC."
            )
            : minimum;

    // GC counts are process-wide, so a collection from another thread invalidates the sample.
    internal static async ValueTask<(bool IsValid, T Value)> MeasureWindowAsync<T>(
        Func<ValueTask<T>> measure,
        Action? beforeWindow = null,
        Action? afterWindow = null
    )
    {
        const int attempts = 4;
        for (var attempt = 0; attempt < attempts; attempt++)
        {
            beforeWindow?.Invoke();
            var collectionCounts = CollectionCounts();
            try
            {
                var result = await measure();
                if (CollectionCountsUnchanged(collectionCounts))
                    return (true, result);
            }
            finally
            {
                afterWindow?.Invoke();
            }
        }
        return (false, default!);
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
            var measurement = MeasureWindowAsync(
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
            if (!measurement.IsValid)
                continue;
            minimum = Math.Min(minimum, measurement.Value);
        }
        return RequireMinimum(minimum);
    }

    /// <summary>
    ///     Two actions measured in alternating windows, so that both see the same JIT tiering
    ///     and cache state, for tests that compare one against the other with Equal. Both
    ///     actions share a window, which is discarded if a GC overlaps either action.
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
            var measurement = MeasureWindowAsync(() =>
                {
                    var before = GC.GetAllocatedBytesForCurrentThread();
                    for (var index = 0; index < callsPerWindow; index++)
                        first(index);
                    var firstBytes = GC.GetAllocatedBytesForCurrentThread() - before;
                    before = GC.GetAllocatedBytesForCurrentThread();
                    for (var index = 0; index < callsPerWindow; index++)
                        second(index);
                    var secondBytes = GC.GetAllocatedBytesForCurrentThread() - before;
                    return new ValueTask<(long First, long Second)>((firstBytes, secondBytes));
                })
                .GetAwaiter()
                .GetResult();
            if (!measurement.IsValid)
                continue;
            minimumFirst = Math.Min(minimumFirst, measurement.Value.First);
            minimumSecond = Math.Min(minimumSecond, measurement.Value.Second);
        }
        RequireMinimum(minimumFirst);
        RequireMinimum(minimumSecond);
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
            var measurement = await MeasureWindowAsync(async () =>
            {
                var before = GC.GetAllocatedBytesForCurrentThread();
                for (var index = 0; index < callsPerWindow; index++)
                    await action();
                return GC.GetAllocatedBytesForCurrentThread() - before;
            });
            if (!measurement.IsValid)
                continue;
            minimum = Math.Min(minimum, measurement.Value);
        }
        return RequireMinimum(minimum);
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
