namespace Zlink.Framework.UnitTests;

/// <summary>
///     Measures the bytes one call allocates on the current thread. The result is the
///     per-call value that most pause-free calls allocate, never a window total or mean.
///     An extra allocation in the code under test adds to every call and moves that value.
///     A one-time runtime allocation (tier-up, a lazy runtime table, storage growth) lands
///     in one call only and cannot decide a strict comparison.
/// </summary>
internal static class PerCallAllocation
{
    public const int Calls = 1000;

    public static long Steady(Action action, int calls = Calls, int warmup = 0)
    {
        for (var index = 0; index < warmup; index++)
            action();
        return Steady(_ => action(), calls);
    }

    public static long Steady(Action<int> action, int calls = Calls)
    {
        var counts = new Dictionary<long, int>();
        for (var index = 0; index < calls; index++)
        {
            var pause = GC.GetTotalPauseDuration();
            var before = GC.GetAllocatedBytesForCurrentThread();
            action(index);
            var bytes = GC.GetAllocatedBytesForCurrentThread() - before;
            Record(counts, bytes, pause);
        }
        return Result(counts);
    }

    public static async Task<long> SteadyAsync(Func<ValueTask> action, int calls = Calls)
    {
        var counts = new Dictionary<long, int>();
        for (var index = 0; index < calls; index++)
        {
            var pause = GC.GetTotalPauseDuration();
            var before = GC.GetAllocatedBytesForCurrentThread();
            await action();
            var bytes = GC.GetAllocatedBytesForCurrentThread() - before;
            Record(counts, bytes, pause);
        }
        return Result(counts);
    }

    // A GC pause inside a call can retire the thread allocation context and add its
    // unused tail to the thread counter, so a call that overlapped a pause is skipped.
    private static void Record(Dictionary<long, int> counts, long bytes, TimeSpan pauseBefore)
    {
        if (GC.GetTotalPauseDuration() != pauseBefore)
            return;
        counts[bytes] = counts.GetValueOrDefault(bytes) + 1;
    }

    /// <summary>
    ///     The most frequent value of per-call samples a test collected itself. On a tie, the
    ///     larger value, so a real per-call cost is never hidden.
    /// </summary>
    public static long Mode(IEnumerable<long> samples)
    {
        var counts = new Dictionary<long, int>();
        foreach (var bytes in samples)
            counts[bytes] = counts.GetValueOrDefault(bytes) + 1;
        return Result(counts);
    }

    private static long Result(Dictionary<long, int> counts) =>
        counts.Count == 0
            ? throw new InvalidOperationException("Every measured call overlapped a GC pause.")
            : counts.MaxBy(static entry => (entry.Value, entry.Key)).Key;
}
