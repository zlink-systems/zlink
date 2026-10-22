using System.Diagnostics;
using System.Runtime.CompilerServices;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RequestCompletionCancellationTests
{
    // Public entry used by the same-process snapshot comparison. Both contexts
    // load this identical test DLL; only their Framework DLL differs.
    public static double[] MeasureRegistration(int count, bool cancellable)
    {
        using var source = new CancellationTokenSource();
        var token = cancellable ? source.Token : CancellationToken.None;
        using var process = Process.GetCurrentProcess();
        var cpu = process.TotalProcessorTime.Ticks;
        var gen0 = GC.CollectionCount(0);
        var gen1 = GC.CollectionCount(1);
        var gen2 = GC.CollectionCount(2);
        var allocated = GC.GetAllocatedBytesForCurrentThread();
        var started = Stopwatch.GetTimestamp();
        for (var index = 0; index < count; index++)
        {
            using var completion = new ZLinkRequestCompletion<int>(token);
        }
        var ticks = Stopwatch.GetTimestamp() - started;
        var bytes = GC.GetAllocatedBytesForCurrentThread() - allocated;
        var cpuTicks = process.TotalProcessorTime.Ticks - cpu;
        return
        [
            ticks * 1_000_000_000.0 / Stopwatch.Frequency / count,
            (double)bytes / count,
            cpuTicks / (double)TimeSpan.TicksPerMillisecond,
            GC.CollectionCount(0) - gen0,
            GC.CollectionCount(1) - gen1,
            GC.CollectionCount(2) - gen2,
        ];
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Cancellation_registration_allocates_only_standard_runtime_storage(bool cancellable)
    {
        using var source = new CancellationTokenSource();
        var token = cancellable ? source.Token : CancellationToken.None;
        for (var index = 0; index < 256; index++)
        {
            Baseline(token);
            using var completion = new ZLinkRequestCompletion<int>(token);
        }
        var baseline = SteadyAllocated(() => Baseline(token));
        var actual = SteadyAllocated(() =>
        {
            using var completion = new ZLinkRequestCompletion<int>(token);
        });
        Assert.Equal(baseline, actual);
    }

    // Steady per-call bytes: the value most pause-free calls allocate. An extra
    // allocation in the completion adds to every call, while a one-time runtime
    // allocation in either window (registration storage growth, tier-up) adds to
    // one call only and must not decide the comparison.
    private static long SteadyAllocated(Action action)
    {
        const int count = 1024;
        var counts = new Dictionary<long, int>();
        for (var measured = 0; measured < count; )
        {
            var pause = GC.GetTotalPauseDuration();
            var before = GC.GetAllocatedBytesForCurrentThread();
            action();
            var bytes = GC.GetAllocatedBytesForCurrentThread() - before;
            if (GC.GetTotalPauseDuration() != pause)
                continue;
            counts[bytes] = counts.GetValueOrDefault(bytes) + 1;
            measured++;
        }
        // On a tie, report the larger count so a real per-call cost is never hidden.
        return counts.MaxBy(static entry => (entry.Value, entry.Key)).Key;
    }

    private static void Baseline(CancellationToken token)
    {
        // Measure this runtime's actual storage size without duplicating the
        // completion's field layout. The target loop executes its normal ctor.
        var storage = RuntimeHelpers.GetUninitializedObject(typeof(ZLinkRequestCompletion<int>));
        var terminal = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var cancellation = token.Register(static (_, _) => { }, terminal);
        using var shutdown = CancellationToken.None.Register(static _ => { }, terminal);
        GC.KeepAlive(storage);
        GC.KeepAlive(terminal);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Cancellation_preserves_token_and_discards_late_result(bool alreadyCanceled)
    {
        using var source = new CancellationTokenSource();
        if (alreadyCanceled)
            source.Cancel();
        var discarded = 0;
        using var completion = new ZLinkRequestCompletion<int>(
            source.Token,
            discardResult: value => discarded += value
        );
        if (!alreadyCanceled)
            source.Cancel();
        var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => completion.Task);
        Assert.Equal(source.Token, error.CancellationToken);
        completion.Complete(7);
        Assert.Equal(7, discarded);
        Assert.True(completion.Task.IsCanceled);
    }

    [Fact]
    public async Task None_token_completes_normally()
    {
        using var completion = new ZLinkRequestCompletion<int>(CancellationToken.None);
        completion.Complete(7);
        Assert.Equal(7, await completion.Task);
    }

    [Fact]
    public async Task Disposal_unregisters_cancellation()
    {
        using var source = new CancellationTokenSource();
        var completion = new ZLinkRequestCompletion<int>(source.Token);
        completion.Dispose();
        source.Cancel();
        Assert.False(completion.Task.IsCompleted);
        completion.Complete(7);
        Assert.Equal(7, await completion.Task);
    }
}
