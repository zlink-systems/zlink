namespace ZLink.Framework.Perf.Tests;

internal static class TestWorkload
{
    public static Workload Create(
        double warmupSeconds = .05,
        double durationSeconds = .05,
        int drainTimeoutMs = 1_000,
        int setupTimeoutMs = 30_000,
        int? connections = null,
        int? logicalStreams = 1
    ) =>
        new(
            1024,
            durationSeconds,
            warmupSeconds,
            connections,
            logicalStreams,
            1,
            connections,
            drainTimeoutMs,
            setupTimeoutMs,
            5_000,
            1_000
        );
}
