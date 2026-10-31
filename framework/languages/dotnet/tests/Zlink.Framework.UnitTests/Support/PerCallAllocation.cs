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

    // Background GC can charge unused allocation-context space to this thread's counter
    // (dotnet/runtime#134724). Reserve space before measuring instead of discarding
    // windows whose own allocations cause GC. Exhaustion still fails the measurement.
    private readonly struct MeasurementRegion : IDisposable
    {
        public MeasurementRegion()
        {
            if (!GC.TryStartNoGCRegion(8 * 1024 * 1024))
                throw new InvalidOperationException(
                    "Could not reserve a GC-free measurement window."
                );
        }

        public void Dispose() => GC.EndNoGCRegion();
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
            beforeWindow?.Invoke(window);
            long bytes;
            using (new MeasurementRegion())
            {
                var before = GC.GetAllocatedBytesForCurrentThread();
                for (var index = 0; index < callsPerWindow; index++)
                    action(index);
                bytes = GC.GetAllocatedBytesForCurrentThread() - before;
            }
            afterWindow?.Invoke(window);
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
            using var region = new MeasurementRegion();
            var before = GC.GetAllocatedBytesForCurrentThread();
            for (var index = 0; index < callsPerWindow; index++)
                first(index);
            var firstBytes = GC.GetAllocatedBytesForCurrentThread() - before;
            before = GC.GetAllocatedBytesForCurrentThread();
            for (var index = 0; index < callsPerWindow; index++)
                second(index);
            var secondBytes = GC.GetAllocatedBytesForCurrentThread() - before;
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
            using var region = new MeasurementRegion();
            var before = GC.GetAllocatedBytesForCurrentThread();
            for (var index = 0; index < callsPerWindow; index++)
                await action();
            var bytes = GC.GetAllocatedBytesForCurrentThread() - before;
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
