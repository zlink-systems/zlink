using System.Diagnostics;
using System.Diagnostics.Tracing;
using System.Reflection;
using System.Runtime.CompilerServices;
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
            for (var index = 0; index < ZLinkMeshDispatchPump.ApplicationWorkerCount; index++)
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

        Assert.Equal(
            SignalCount + ZLinkMeshDispatchPump.ApplicationWorkerCount,
            waits.ReadinessWaits
        );
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
        for (var index = 0; index < ZLinkMeshDispatchPump.ApplicationWorkerCount; index++)
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
        var workerCount = ZLinkMeshDispatchPump.ApplicationWorkerCount;
        await using var context = Systems.Zlink.Zlink.CreateContext();
        using var waits = new WorkerWaitListener();
        for (var iteration = 0; iteration < iterations; iteration++)
        {
            await using var node = new ZLinkManagedMeshNode(context, "worker-stop-race");
            var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable());
            try
            {
                waits.Start();
                pump.EnsureStarted();
                for (var worker = 0; worker < workerCount; worker++)
                    await waits.NextWaitAsync().WaitAsync(TestGuard);
            }
            finally
            {
                waits.Stop();
                await pump.DisposeAsync().AsTask().WaitAsync(TestGuard);
            }
        }
        output.WriteLine(
            $"STOP iterations={iterations} workers={workerCount} elapsed={elapsed.Elapsed} hung=0"
        );
        Assert.Equal(iterations * workerCount, waits.ReadinessWaits);
        Assert.Equal(0, waits.CancellationWrappers);
    }

    [Fact]
    public async Task WorkerCompletionDoesNotCountInlineCallerWaitAsReadiness()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "worker-completion-wait");
        await using var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable());
        using var waits = new WorkerWaitListener();
        waits.Start();
        pump.EnsureStarted();
        for (var worker = 0; worker < ZLinkMeshDispatchPump.ApplicationWorkerCount; worker++)
            await waits.NextWaitAsync().WaitAsync(TestGuard);
        waits.Stop();
        var workers = (Task)
            typeof(ZLinkMeshDispatchPump)
                .GetField("_loop", BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(pump)!;
        var resumeCaller = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        Task? callerWait = null;
        var continuation = workers.ContinueWith(
            _ =>
            {
                // Force the next await to run inline under the completing worker,
                // just as the repeated-stop test can resume after DisposeAsync.
                Assert.Contains(
                    new StackTrace().GetFrames(),
                    frame =>
                    {
                        var type = frame.GetMethod()?.DeclaringType;
                        return type?.DeclaringType == typeof(ZLinkMeshDispatchPump)
                            && type.Name.StartsWith("<RunAsync>", StringComparison.Ordinal);
                    }
                );
                waits.Start();
                try
                {
                    callerWait = AwaitCallerAsync(resumeCaller.Task);
                }
                finally
                {
                    waits.Stop();
                }
            },
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default
        );
        try
        {
            await pump.DisposeAsync().AsTask().WaitAsync(TestGuard);
            await continuation.WaitAsync(TestGuard);
            Assert.Equal(ZLinkMeshDispatchPump.ApplicationWorkerCount, waits.ReadinessWaits);
            Assert.Equal(0, waits.CancellationWrappers);
        }
        finally
        {
            resumeCaller.TrySetResult();
            if (callerWait is not null)
                await callerWait.WaitAsync(TestGuard);
        }

        static async Task AwaitCallerAsync(Task resume) => await resume;
    }

    [Fact]
    public async Task DisposeUnregistersPendingStopWakeBeforeDisposingSignal()
    {
        var node = DispatchProxy.Create<IMeshNode, PausedDrainNode>();
        var drains = (PausedDrainNode)(object)node;
        var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable());
        pump.EnsureStarted();
        var stop = (CancellationTokenSource)
            typeof(ZLinkMeshDispatchPump)
                .GetField("_stop", BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(pump)!;
        var cancellationEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var resumeCancellation = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        Task? cancelling = null;
        CancellationTokenRegistration checkpoint = default;
        try
        {
            for (var worker = 0; worker < ZLinkMeshDispatchPump.ApplicationWorkerCount; worker++)
            {
                drains.Ready!(MeshReadyDomains.Application);
                await drains.NextDrainAsync().WaitAsync(TestGuard);
            }
            // Cancellation registrations run newest first. Hold cancellation after
            // its flag is set but before the pump's stop-wake callback can run.
            checkpoint = stop.Token.Register(() =>
            {
                cancellationEntered.SetResult();
                resumeCancellation.Task.GetAwaiter().GetResult();
            });
            cancelling = Task.Run(stop.Cancel);
            await cancellationEntered.Task.WaitAsync(TestGuard);
            drains.Resume();
            // Every worker leaves its drain and observes cancellation without a
            // semaphore wake. Disposal must retire the pending wake registration.
            await pump.DisposeAsync().AsTask().WaitAsync(TestGuard);
            resumeCancellation.SetResult();
            await cancelling.WaitAsync(TestGuard);
        }
        finally
        {
            drains.Resume();
            resumeCancellation.TrySetResult();
            if (cancelling is not null)
                await cancelling.WaitAsync(TestGuard);
            checkpoint.Dispose();
            await pump.DisposeAsync().AsTask().WaitAsync(TestGuard);
        }
    }

    private class PausedDrainNode : DispatchProxy
    {
        private readonly Channel<bool> _entered = Channel.CreateUnbounded<bool>();
        private readonly TaskCompletionSource<bool> _resume = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        internal Func<MeshReadyDomains, MeshReadyDomains>? Ready { get; private set; }

        internal Task NextDrainAsync() => _entered.Reader.ReadAsync().AsTask();

        internal void Resume() => _resume.TrySetResult(false);

        protected override object? Invoke(MethodInfo? method, object?[]? arguments)
        {
            switch (method!.Name)
            {
                case nameof(IMeshNode.SetReadyHandler):
                    Ready = (Func<MeshReadyDomains, MeshReadyDomains>)arguments![0]!;
                    return null;
                case nameof(IMeshNode.DrainReadyAsync):
                    _entered.Writer.TryWrite(true);
                    return new ValueTask<bool>(_resume.Task);
                case nameof(IMeshNode.CloseSubmissions):
                    return null;
                default:
                    throw new NotSupportedException(method.Name);
            }
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
            // A worker completing can inline DisposeAsync and then the test's
            // next await. Attribute the event to the innermost async method,
            // not a completed RunAsync still present farther down the stack.
            // SemaphoreSlim's cancellation wrapper belongs to its caller's wait.
            var waiter = frames
                .Select(frame => frame.GetMethod()?.DeclaringType)
                .FirstOrDefault(type =>
                    type is not null
                    && typeof(IAsyncStateMachine).IsAssignableFrom(type)
                    && type.DeclaringType != typeof(SemaphoreSlim)
                );
            if (
                waiter?.DeclaringType != typeof(ZLinkMeshDispatchPump)
                || !waiter.Name.StartsWith("<RunAsync>", StringComparison.Ordinal)
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
