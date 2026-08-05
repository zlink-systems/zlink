using System.Diagnostics;

namespace Zlink.Framework.Runtime.Dispatch;

// The transport-liveness contract bounds one receive turn on all three axes.
// A first record is admitted even when it is larger than the byte limit; an
// individual record cannot be split by this layer.
internal static class ZLinkReceiveBatchBudget
{
    internal const int MaximumRecords = 64;
    internal const long MaximumBytes = 4L * 1024 * 1024;
    internal const int MaximumMilliseconds = 1;

    internal static bool IsExhausted(
        int records,
        long bytes,
        long startedAt)
    {
        return records >= MaximumRecords
               || bytes >= MaximumBytes
               || ElapsedMilliseconds(startedAt) >= MaximumMilliseconds;
    }

    internal static bool WouldExceed(
        int records,
        long bytes,
        long nextBytes,
        long startedAt)
    {
        if (records == 0) return false;
        if (IsExhausted(records, bytes, startedAt)) return true;
        return nextBytes > 0
               && nextBytes > MaximumBytes - Math.Min(bytes, MaximumBytes);
    }

    internal static long MeasureParts(IReadOnlyList<Message> parts)
    {
        long total = 0;
        foreach (var part in parts)
            total = checked(total + Math.Max(part.Size, 0));
        return total;
    }

    private static long ElapsedMilliseconds(long startedAt)
    {
        var elapsedTicks = Stopwatch.GetTimestamp() - startedAt;
        if (elapsedTicks <= 0) return 0;
        return elapsedTicks * 1000L / Stopwatch.Frequency;
    }
}
