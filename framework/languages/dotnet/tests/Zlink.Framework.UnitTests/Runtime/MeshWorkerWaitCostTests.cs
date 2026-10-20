using System.Diagnostics;
using System.Diagnostics.Tracing;
using System.Reflection;
using System.Threading.Channels;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshWorkerWaitCostTests
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
        using var stop = new CancellationTokenSource();
        using var runtime = new CancellationTokenSource();
        using var waits = new WorkerWaitListener();
        // Isolate one persistent worker with the existing Framework test convention
        // of invoking a private ingress/worker boundary. No binding internals are used.
        var run = typeof(ZLinkMeshDispatchPump).GetMethod(
            "RunAsync",
            BindingFlags.Instance | BindingFlags.NonPublic
        )!;
        using var lifetime = CancellationTokenSource.CreateLinkedTokenSource(
            stop.Token,
            runtime.Token
        );
        var worker = ((ValueTask)run.Invoke(pump, [lifetime.Token])!).AsTask();
        var signal = typeof(ZLinkMeshDispatchPump).GetMethod(
            "SignalReady",
            BindingFlags.Instance | BindingFlags.NonPublic
        )!;
        try
        {
            await waits.NextWaitAsync().WaitAsync(TestGuard);
            for (var index = 0; index < SignalCount; index++)
            {
                signal.Invoke(pump, [MeshReadyDomains.Application]);
                await waits.NextWaitAsync().WaitAsync(TestGuard);
            }
        }
        finally
        {
            (runtimeStops ? runtime : stop).Cancel();
            await worker.WaitAsync(TestGuard);
        }

        Assert.Equal(SignalCount + 1, waits.ReadinessWaits);
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
        pump.EnsureStarted();
        for (var index = 0; index < Math.Max(2, Environment.ProcessorCount); index++)
            await waits.NextWaitAsync().WaitAsync(TestGuard);
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

        internal WorkerWaitListener()
        {
            foreach (var source in EventSource.GetSources())
                Enable(source);
        }

        internal Task NextWaitAsync() => _waiting.Reader.ReadAsync().AsTask();

        protected override void OnEventSourceCreated(EventSource source) => Enable(source);

        private void Enable(EventSource source)
        {
            if (source.Name == "System.Threading.Tasks.TplEventSource")
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
