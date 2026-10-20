using System.Diagnostics;
using System.Diagnostics.Tracing;
using System.Reflection;
using System.Threading.Channels;
using Xunit.Abstractions;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshWorkerWaitCostTests(ITestOutputHelper output)
{
    private const int SignalCount = 1000;
    private static readonly TimeSpan TestGuard = TimeSpan.FromSeconds(5);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task PersistentWorkerDoesNotWrapEachReadinessWaitForCancellation(bool runtimeStops)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "worker-wait-cost");
        await using var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable());
        using var runtime = new CancellationTokenSource();
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, runtime.Token);
        pump.SetNodeRouteHandler(static (_, _) => ValueTask.CompletedTask, runner);
        using var waits = new WorkerWaitListener();
        waits.Start();
        pump.EnsureStarted();
        var signal = typeof(ZLinkMeshDispatchPump).GetMethod(
            "SignalReady",
            BindingFlags.Instance | BindingFlags.NonPublic
        )!;
        try
        {
            for (var index = 0; index < Math.Max(2, Environment.ProcessorCount); index++)
                await waits.NextWaitAsync().WaitAsync(TestGuard);
            for (var index = 0; index < SignalCount; index++)
            {
                signal.Invoke(pump, [MeshReadyDomains.Application]);
                await waits.NextWaitAsync().WaitAsync(TestGuard);
            }
        }
        finally
        {
            waits.Stop();
            if (runtimeStops)
                runtime.Cancel();
            await pump.DisposeAsync().AsTask().WaitAsync(TestGuard);
            await runner.StopAsync().AsTask().WaitAsync(TestGuard);
        }

        Assert.Equal(SignalCount + Math.Max(2, Environment.ProcessorCount), waits.ReadinessWaits);
        Assert.Equal(0, waits.CancellationWrappers);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SharedWorkersExitWhenTheirLifetimeStops(bool runtimeStops)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "worker-stop");
        using var runtime = new CancellationTokenSource();
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, runtime.Token);
        await using var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable());
        pump.SetNodeRouteHandler(static (_, _) => ValueTask.CompletedTask, runner);
        using var waits = new WorkerWaitListener();
        waits.Start();
        pump.EnsureStarted();
        for (var index = 0; index < Math.Max(2, Environment.ProcessorCount); index++)
            await waits.NextWaitAsync().WaitAsync(TestGuard);
        waits.Stop();
        try
        {
            if (runtimeStops)
            {
                runtime.Cancel();
                await runner.StopAsync().AsTask().WaitAsync(TestGuard);
            }
            else
            {
                await pump.DisposeAsync().AsTask().WaitAsync(TestGuard);
                await runner.StopAsync().AsTask().WaitAsync(TestGuard);
            }
        }
        finally
        {
            runtime.Cancel();
            await pump.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Fact]
    public async Task RepeatedStopWakesEveryWaitingWorker()
    {
        const int iterations = 3000;
        var elapsed = Stopwatch.StartNew();
        var workerCount = Math.Max(2, Environment.ProcessorCount);
        await using var context = Systems.Zlink.Zlink.CreateContext();
        using var waits = new WorkerWaitListener();
        for (var iteration = 0; iteration < iterations; iteration++)
        {
            await using var node = new ZLinkManagedMeshNode(context, "worker-stop-race");
            var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable());
            Task? stopped = null;
            try
            {
                waits.Start();
                pump.EnsureStarted();
                for (var worker = 0; worker < workerCount; worker++)
                    await waits.NextWaitAsync().WaitAsync(TestGuard);
                waits.Stop();
                stopped = pump.DisposeAsync().AsTask();
                await stopped.WaitAsync(TestGuard);
            }
            finally
            {
                waits.Stop();
                stopped ??= pump.DisposeAsync().AsTask();
                if (!stopped.IsCompleted)
                {
                    output.WriteLine($"Stop did not complete at iteration {iteration}.");
                    // Recover only after the test has already failed its stop guard,
                    // so the failed worker cannot leak into later measurements.
                    var signal = (SemaphoreSlim)
                        typeof(ZLinkMeshDispatchPump)
                            .GetField("_signal", BindingFlags.Instance | BindingFlags.NonPublic)!
                            .GetValue(pump)!;
                    signal.Release(workerCount);
                    await stopped.WaitAsync(TestGuard);
                }
            }
        }
        output.WriteLine(
            $"STOP iterations={iterations} workers={workerCount} elapsed={elapsed.Elapsed} hung=0"
        );
        Assert.Equal(iterations * workerCount, waits.ReadinessWaits);
        Assert.Equal(0, waits.CancellationWrappers);
    }

    private sealed class WorkerWaitListener : EventListener
    {
        private readonly Channel<bool> _waiting = Channel.CreateUnbounded<bool>(
            new UnboundedChannelOptions
            {
                AllowSynchronousContinuations = false,
                SingleReader = true,
            }
        );
        private int _readinessWaits;
        private int _cancellationWrappers;

        internal int ReadinessWaits => Volatile.Read(ref _readinessWaits);
        internal int CancellationWrappers => Volatile.Read(ref _cancellationWrappers);

        private bool _enabled;

        internal void Start()
        {
            _enabled = true;
            foreach (var source in EventSource.GetSources())
                Enable(source);
        }

        internal void Stop()
        {
            _enabled = false;
            foreach (var source in EventSource.GetSources())
                if (source.Name == "System.Threading.Tasks.TplEventSource")
                    DisableEvents(source);
        }

        internal Task NextWaitAsync() => _waiting.Reader.ReadAsync().AsTask();

        protected override void OnEventSourceCreated(EventSource source) => Enable(source);

        private void Enable(EventSource source)
        {
            if (_enabled && source.Name == "System.Threading.Tasks.TplEventSource")
                EnableEvents(source, EventLevel.Verbose, (EventKeywords)3);
        }

        protected override void OnEventWritten(EventWrittenEventArgs data)
        {
            if (data.EventName != "TaskWaitBegin")
                return;
            var frames = new StackTrace().GetFrames();
            if (
                !frames.Any(frame =>
                {
                    var type = frame.GetMethod()?.DeclaringType;
                    return type?.DeclaringType == typeof(ZLinkMeshDispatchPump)
                        && type.Name.StartsWith("<RunAsync>", StringComparison.Ordinal);
                })
            )
                return;
            if (
                frames.Any(frame =>
                    frame.GetMethod()?.DeclaringType == typeof(SemaphoreSlim)
                    && frame.GetMethod()?.Name == "WaitUntilCountOrTimeoutAsync"
                )
            )
            {
                Interlocked.Increment(ref _cancellationWrappers);
                return;
            }
            Interlocked.Increment(ref _readinessWaits);
            _waiting.Writer.TryWrite(true);
        }
    }
}
