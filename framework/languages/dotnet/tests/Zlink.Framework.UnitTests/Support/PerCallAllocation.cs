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

    // A window that overlapped a GC pause is discarded and measured again, up to this
    // many attempts per requested window.
    private const int AttemptsPerWindow = 4;

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
    ///     measured region with the attempt number, to create and release per-window state.
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
        var measured = 0;
        for (var attempt = 0; measured < windows; attempt++)
        {
            if (attempt >= windows * AttemptsPerWindow)
                throw new InvalidOperationException("Every measured window overlapped a GC pause.");
            beforeWindow?.Invoke(attempt);
            var pause = GC.GetTotalPauseDuration();
            var before = GC.GetAllocatedBytesForCurrentThread();
            for (var index = 0; index < callsPerWindow; index++)
                action(index);
            var bytes = GC.GetAllocatedBytesForCurrentThread() - before;
            var paused = GC.GetTotalPauseDuration() != pause;
            afterWindow?.Invoke(attempt);
            if (paused)
                continue;
            minimum = Math.Min(minimum, bytes);
            measured++;
        }
        return minimum;
    }

    /// <summary>
    ///     Two actions measured in alternating windows, so that both see the same JIT tiering
    ///     and cache state, for tests that compare one against the other with Equal. A round
    ///     counts only when neither window overlapped a GC pause.
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
        var measured = 0;
        for (var attempt = 0; measured < windows; attempt++)
        {
            if (attempt >= windows * AttemptsPerWindow)
                throw new InvalidOperationException("Every measured window overlapped a GC pause.");
            var pause = GC.GetTotalPauseDuration();
            var before = GC.GetAllocatedBytesForCurrentThread();
            for (var index = 0; index < callsPerWindow; index++)
                first(index);
            var firstBytes = GC.GetAllocatedBytesForCurrentThread() - before;
            var middlePause = GC.GetTotalPauseDuration();
            before = GC.GetAllocatedBytesForCurrentThread();
            for (var index = 0; index < callsPerWindow; index++)
                second(index);
            var secondBytes = GC.GetAllocatedBytesForCurrentThread() - before;
            if (middlePause != pause || GC.GetTotalPauseDuration() != pause)
                continue;
            minimumFirst = Math.Min(minimumFirst, firstBytes);
            minimumSecond = Math.Min(minimumSecond, secondBytes);
            measured++;
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
        var measured = 0;
        for (var attempt = 0; measured < windows; attempt++)
        {
            if (attempt >= windows * AttemptsPerWindow)
                throw new InvalidOperationException("Every measured window overlapped a GC pause.");
            var pause = GC.GetTotalPauseDuration();
            var before = GC.GetAllocatedBytesForCurrentThread();
            for (var index = 0; index < callsPerWindow; index++)
                await action();
            var bytes = GC.GetAllocatedBytesForCurrentThread() - before;
            if (GC.GetTotalPauseDuration() != pause)
                continue;
            minimum = Math.Min(minimum, bytes);
            measured++;
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
