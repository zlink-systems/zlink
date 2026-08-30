using System.Diagnostics;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_perf_multi_admission_signal
{
    [Fact]
    public async Task tracked_admission_completion_wakes_waiter()
    {
        var signal = new global::PerfMultiAdmissionSignal();
        var admission = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        signal.Track(admission.Task);

        Task<bool> waiting = signal.WaitAsync(Deadline()).AsTask();
        Assert.False(waiting.IsCompleted);

        admission.SetResult();

        Assert.True(await waiting.WaitAsync(TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public async Task completion_before_wait_is_consumed_without_losing_next_wake()
    {
        var signal = new global::PerfMultiAdmissionSignal();
        signal.Track(Task.CompletedTask);

        Assert.True(await signal.WaitAsync(Deadline()));

        var nextAdmission = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        signal.Track(nextAdmission.Task);
        Task<bool> nextWait = signal.WaitAsync(Deadline()).AsTask();
        Assert.False(nextWait.IsCompleted,
            "the preceding completion must not leak into the next generation");

        nextAdmission.SetResult();

        Assert.True(await nextWait.WaitAsync(TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public async Task admission_drain_has_a_bounded_failure_path()
    {
        var admission = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        TimeoutException error = await Assert.ThrowsAsync<TimeoutException>(
            () => global::PerfMultiAdmissionDrain.WaitAsync(
                new[] { admission.Task }, 20));

        Assert.Contains("did not drain", error.Message,
            StringComparison.Ordinal);
        admission.SetResult();
    }

    private static long Deadline()
    {
        return Stopwatch.GetTimestamp() + 5L * Stopwatch.Frequency;
    }
}
