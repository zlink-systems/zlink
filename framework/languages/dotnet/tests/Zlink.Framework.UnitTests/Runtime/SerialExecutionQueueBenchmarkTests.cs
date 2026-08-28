using System.Diagnostics;

using Xunit.Abstractions;

namespace Zlink.Framework.UnitTests;

public sealed class SerialExecutionQueueBenchmarkTests(ITestOutputHelper output)
{
    private const int AdmissionAttempts = 4352;

    [Fact]
    public async Task Default_application_admission_burst_reports_resource_and_recovery_metrics()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None),
            errorSink,
            CancellationToken.None);
        var firstStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var admissionLatencies = new long[AdmissionAttempts];
        var accepted = 0;
        var rejected = 0;
        var firstRejectionOrdinal = 0;

        var firstStartedAt = Stopwatch.GetTimestamp();
        Assert.True(queue.TryPost(
            async _ =>
            {
                firstStarted.TrySetResult();
                await releaseFirst.Task.ConfigureAwait(false);
            },
            out _));
        admissionLatencies[0] = Stopwatch.GetTimestamp() - firstStartedAt;
        accepted++;
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var burstStartedAt = Stopwatch.GetTimestamp();
        for (var index = 1; index < AdmissionAttempts; index++)
        {
            var startedAt = Stopwatch.GetTimestamp();
            if (queue.TryPost(
                    static _ => ValueTask.CompletedTask,
                    out _))
            {
                accepted++;
            }
            else
            {
                rejected++;
                if (firstRejectionOrdinal == 0)
                    firstRejectionOrdinal = index + 1;
            }
            admissionLatencies[index] = Stopwatch.GetTimestamp() - startedAt;
        }
        var burstElapsed = Stopwatch.GetElapsedTime(burstStartedAt);
        var peakPending = queue.ApplicationPendingCount;

        var recoveryStartedAt = Stopwatch.GetTimestamp();
        releaseFirst.TrySetResult();
        while (!queue.TryPost(
                   static _ => ValueTask.CompletedTask,
                   out _))
            await Task.Yield();
        var firstReadmission = Stopwatch.GetElapsedTime(recoveryStartedAt);
        await queue.ApplicationDrained.WaitAsync(TimeSpan.FromSeconds(10));
        var fullyDrained = Stopwatch.GetElapsedTime(recoveryStartedAt);

        Array.Sort(admissionLatencies);
        var p95 = Percentile(admissionLatencies, 0.95);
        var p99 = Percentile(admissionLatencies, 0.99);
        var throughput = AdmissionAttempts / burstElapsed.TotalSeconds;

        output.WriteLine(
            "admissionAttempts={0} accepted={1} rejected={2} firstRejectionOrdinal={3}",
            AdmissionAttempts,
            accepted,
            rejected,
            firstRejectionOrdinal);
        output.WriteLine(
            "throughputPerSecond={0:F0} p95Microseconds={1:F3} p99Microseconds={2:F3}",
            throughput,
            TicksToMicroseconds(p95),
            TicksToMicroseconds(p99));
        output.WriteLine(
            "peakPending={0} firstReadmissionMilliseconds={1:F3} fullyDrainedMilliseconds={2:F3}",
            peakPending,
            firstReadmission.TotalMilliseconds,
            fullyDrained.TotalMilliseconds);

        // The serial execution queue reserves both application axes while the
        // handler is pending. The default count limit is reached before the
        // larger benchmark burst is admitted.
        Assert.Equal(AdmissionAttempts, accepted + rejected);
        Assert.Equal(
            AdmissionAttempts - ZLinkExecutionLanePolicy.Default.ApplicationMessageCapacity,
            rejected);
        Assert.Equal(
            ZLinkExecutionLanePolicy.Default.ApplicationMessageCapacity + 1,
            firstRejectionOrdinal);
        Assert.Equal(
            ZLinkExecutionLanePolicy.Default.ApplicationMessageCapacity,
            peakPending);
        Assert.True(firstReadmission >= TimeSpan.Zero);
        Assert.True(fullyDrained >= firstReadmission);
    }

    private static long Percentile(long[] sorted, double percentile)
    {
        var index = (int)Math.Ceiling(sorted.Length * percentile) - 1;
        return sorted[Math.Clamp(index, 0, sorted.Length - 1)];
    }

    private static double TicksToMicroseconds(long ticks) =>
        ticks * 1_000_000d / Stopwatch.Frequency;
}
