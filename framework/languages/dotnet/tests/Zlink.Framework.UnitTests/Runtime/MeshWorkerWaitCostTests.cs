using System.Collections.Concurrent;
using System.Diagnostics;
using System.Reflection;
using System.Threading.Channels;
using Xunit.Abstractions;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests;

public sealed class MeshWorkerWaitCostTests(ITestOutputHelper output)
{
    private const int SignalCount = 1000;
    private static readonly TimeSpan TestGuard = TimeSpan.FromSeconds(5);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task FailedDrainReleasesTheTransferredReadyBatch(bool asynchronous)
    {
        var node = DispatchProxy.Create<IMeshNode, DrainNode>();
        var drains = (DrainNode)(object)node;
        var failure = new InvalidOperationException("failed ready drain");
        var completion = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        drains.Result = asynchronous
            ? new ValueTask<bool>(completion.Task)
            : ValueTask.FromException<bool>(failure);
        var released = 0;
        drains.Prepare = batch =>
            batch.Add(
                new(MeshOwnerKind.Node, MeshReadyDomains.Application, string.Empty, default),
                new MeshClaim { Releaser = () => Interlocked.Increment(ref released) }
            );
        using var errors = new ZLinkRuntimeErrorSink();
        var observed = new TaskCompletionSource<Exception>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        errors.UnhandledCallbackException += error => observed.TrySetResult(error);
        var runner = new ZLinkRuntimeTaskRunner(errors, CancellationToken.None);
        var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable());
        pump.SetNodeRouteHandler(static (_, _) => ValueTask.CompletedTask, runner);
        pump.EnsureStarted();
        try
        {
            drains.Ready!(MeshReadyDomains.Application);
            await drains.NextDrainAsync().WaitAsync(TestGuard);
            if (asynchronous)
                completion.TrySetException(failure);
            Assert.Same(failure, await observed.Task.WaitAsync(TestGuard));
            await pump.DisposeAsync().AsTask().WaitAsync(TestGuard);
            await runner.StopAsync().AsTask().WaitAsync(TestGuard);
            Assert.Equal(1, Volatile.Read(ref released));
        }
        finally
        {
            completion.TrySetException(failure);
            await pump.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ProcessWorkersDrainEverySignalWithinTheFixedCapacity(bool runtimeStops)
    {
        var node = DispatchProxy.Create<IMeshNode, DrainNode>();
        var drains = (DrainNode)(object)node;
        using var runtime = new CancellationTokenSource();
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, runtime.Token);
        await using var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable());
        pump.SetNodeRouteHandler(static (_, _) => ValueTask.CompletedTask, runner);
        pump.EnsureStarted();
        try
        {
            for (var index = 0; index < SignalCount; index++)
            {
                drains.Ready!(MeshReadyDomains.Application);
                Assert.False(await drains.NextDrainAsync().WaitAsync(TestGuard));
            }
        }
        finally
        {
            if (runtimeStops)
                runtime.Cancel();
            await pump.DisposeAsync().AsTask().WaitAsync(TestGuard);
            await runner.StopAsync().AsTask().WaitAsync(TestGuard);
        }
        Assert.Equal(SignalCount, drains.DrainCount);
        Assert.InRange(drains.Threads.Count, 1, ZLinkProcessExecutionPool.WorkerCount);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SharedWorkersExitWhenTheirLifetimeStops(bool runtimeStops)
    {
        var node = DispatchProxy.Create<IMeshNode, DrainNode>();
        var drains = (DrainNode)(object)node;
        using var runtime = new CancellationTokenSource();
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, runtime.Token);
        await using var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable());
        pump.SetNodeRouteHandler(static (_, _) => ValueTask.CompletedTask, runner);
        pump.EnsureStarted();
        drains.Ready!(MeshReadyDomains.Application);
        Assert.False(await drains.NextDrainAsync().WaitAsync(TestGuard));
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
        // Keep one process generation while repeatedly retiring pump lifetimes;
        // the final reference separately verifies physical shutdown and join.
        await using var process = ZLinkProcessExecutionPool.CreateReceiveSource();
        for (var iteration = 0; iteration < iterations; iteration++)
        {
            var node = DispatchProxy.Create<IMeshNode, DrainNode>();
            var drains = (DrainNode)(object)node;
            await using var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable());
            pump.EnsureStarted();
            drains.Ready!(MeshReadyDomains.Application);
            Assert.False(await drains.NextDrainAsync().WaitAsync(TestGuard));
            await pump.DisposeAsync().AsTask().WaitAsync(TestGuard);
            Assert.Equal(1, drains.DrainCount);
        }
        await process.DisposeAsync().AsTask().WaitAsync(TestGuard);
        output.WriteLine(
            $"STOP iterations={iterations} capacity={ZLinkProcessExecutionPool.WorkerCount} elapsed={elapsed.Elapsed} hung=0"
        );
    }

    [Fact]
    public async Task ProcessPoolReservesReceiverWhenAllApplicationSlotsAreOccupiedAndJoinsEveryWorker()
    {
        var applicationCount = ZLinkProcessExecutionPool.WorkerCount - 1;
        using var entered = new CountdownEvent(applicationCount);
        using var release = new ManualResetEventSlim();
        var threads = new ConcurrentDictionary<int, Thread>();
        var failures = new ConcurrentQueue<Exception>();
        var sources = Enumerable
            .Range(0, applicationCount)
            .Select(_ =>
                ZLinkProcessExecutionPool.CreateApplicationSource(
                    _ =>
                    {
                        Assert.False(Thread.CurrentThread.IsThreadPoolThread);
                        threads.TryAdd(Environment.CurrentManagedThreadId, Thread.CurrentThread);
                        entered.Signal();
                        Assert.True(release.Wait(TestGuard));
                        return ValueTask.CompletedTask;
                    },
                    failures.Enqueue
                )
            )
            .ToArray();
        var receiver = ZLinkProcessExecutionPool.CreateReceiveSource();
        try
        {
            foreach (var source in sources)
                source.Signal();
            Assert.True(entered.Wait(TestGuard));
            Assert.Equal(applicationCount, threads.Count);
            await ObserveReceiverAsync().WaitAsync(TestGuard);
            Assert.Equal(ZLinkProcessExecutionPool.WorkerCount, threads.Count);
            Assert.False(release.IsSet);
        }
        finally
        {
            release.Set();
            foreach (var source in sources)
                await source.DisposeAsync();
            await receiver.DisposeAsync().AsTask().WaitAsync(TestGuard);
        }
        Assert.Empty(failures);
        Assert.All(threads.Values, thread => Assert.False(thread.IsAlive));

        async Task ObserveReceiverAsync()
        {
            await receiver.ResumeAsync();
            Assert.False(Thread.CurrentThread.IsThreadPoolThread);
            Assert.True(threads.TryAdd(Environment.CurrentManagedThreadId, Thread.CurrentThread));
        }
    }

    [Fact]
    public async Task ClosingAnApplicationSourceDrainsItsAcceptedReadyReservation()
    {
        await using var occupied = new OccupiedApplicationWorkers();
        occupied.Enter();
        var calls = 0;
        var source = ZLinkProcessExecutionPool.CreateApplicationSource(
            _ =>
            {
                Interlocked.Increment(ref calls);
                return ValueTask.CompletedTask;
            },
            error => Assert.Fail(error.ToString())
        );
        source.Signal();
        var disposal = source.DisposeAsync().AsTask();
        occupied.Release();
        await disposal.WaitAsync(TestGuard);
        await occupied.DisposeAsync();
        Assert.Equal(1, calls);
    }

    [Fact]
    public async Task PoolFailureTerminatesAClosingSourceWithAnAcceptedReadyReservation()
    {
        var failure = new InvalidOperationException("Injected failure with a closing source.");
        var poller = new MeshReceiveWakeTests.ControlledReceivePoller();
        var receiver = ZLinkProcessExecutionPool.CreateReceiveSource(() => poller);
        await using var occupied = new OccupiedApplicationWorkers(failure);
        try
        {
            await poller.FirstWait.Task.WaitAsync(TestGuard);
            occupied.Enter();
            var source = ZLinkProcessExecutionPool.CreateApplicationSource(
                _ => throw new InvalidOperationException("A failed pool must not run this turn."),
                _ => { }
            );
            source.Signal();
            var disposal = source.DisposeAsync().AsTask();
            poller.BeforeWait = () => throw failure;
            poller.AllowWait();
            Assert.Same(
                failure,
                await Assert.ThrowsAsync<InvalidOperationException>(() =>
                    disposal.WaitAsync(TestGuard)
                )
            );
        }
        finally
        {
            occupied.Release();
            poller.ReleaseWait();
            await Assert.ThrowsAsync<InvalidOperationException>(() =>
                receiver.DisposeAsync().AsTask()
            );
        }
    }

    [Fact]
    public async Task ClosingTheLastReceiveSourceTerminatesItsPendingWaitBeforeJoining()
    {
        var source = ZLinkProcessExecutionPool.CreateReceiveSource();
        var waiting = source
            .WaitAsync(
                Array.Empty<ZLinkProcessExecutionPool.ReceiveEvent>(),
                Timeout.InfiniteTimeSpan
            )
            .AsTask();
        var disposal = source.DisposeAsync().AsTask();
        await Assert.ThrowsAsync<ObjectDisposedException>(() => waiting.WaitAsync(TestGuard));
        await disposal.WaitAsync(TestGuard);
    }

    [Fact]
    public async Task DisposeJoinsSuspendedPreparationWhenCancellationIsAlreadyInProgress()
    {
        var node = DispatchProxy.Create<IMeshNode, DrainNode>();
        var drains = (DrainNode)(object)node;
        var resume = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        drains.Result = new ValueTask<bool>(resume.Task);
        using var runtime = new CancellationTokenSource();
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, runtime.Token);
        await using var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable());
        pump.SetNodeRouteHandler(static (_, _) => ValueTask.CompletedTask, runner);
        pump.EnsureStarted();
        drains.Ready!(MeshReadyDomains.Application);
        await drains.NextDrainAsync().WaitAsync(TestGuard);
        var cancellationEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var resumeCancellation = new ManualResetEventSlim();
        using var checkpoint = runtime.Token.Register(() =>
        {
            cancellationEntered.TrySetResult();
            Assert.True(resumeCancellation.Wait(TestGuard));
        });
        var cancelling = Task.Run(runtime.Cancel);
        try
        {
            await cancellationEntered.Task.WaitAsync(TestGuard);
            var disposal = pump.DisposeAsync().AsTask();
            Assert.False(disposal.IsCompleted);
            resume.TrySetResult(false);
            await disposal.WaitAsync(TestGuard);
        }
        finally
        {
            resume.TrySetResult(false);
            resumeCancellation.Set();
            await cancelling.WaitAsync(TestGuard);
            await pump.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task FactoryFailureCompletesReceiveAndApplicationOnlySourceLifetimes(
        bool applicationOnly
    )
    {
        var failure = new InvalidOperationException("Injected process factory failure.");
        Func<IPoller> factory = () => throw failure;
        if (applicationOnly)
        {
            var calls = 0;
            var source = ZLinkProcessExecutionPool.CreateApplicationSource(
                _ =>
                {
                    Interlocked.Increment(ref calls);
                    return ValueTask.CompletedTask;
                },
                _ => { },
                factory
            );
            Task? disposal = null;
            try
            {
                Assert.Same(
                    failure,
                    await Assert.ThrowsAsync<InvalidOperationException>(() =>
                        source.Completion.WaitAsync(TestGuard)
                    )
                );
                var disposalTask = source.DisposeAsync().AsTask();
                disposal = disposalTask;
                Assert.Same(
                    failure,
                    await Assert.ThrowsAsync<InvalidOperationException>(() =>
                        disposalTask.WaitAsync(TestGuard)
                    )
                );
                Assert.Equal(0, calls);
            }
            finally
            {
                if (disposal is null)
                    await source.DisposeAsync();
                else if (!disposal.IsCompleted)
                    await disposal;
            }
        }
        else
        {
            var source = ZLinkProcessExecutionPool.CreateReceiveSource(factory);
            Task? disposal = null;
            try
            {
                Assert.Same(
                    failure,
                    await Assert.ThrowsAsync<InvalidOperationException>(async () =>
                        await source.ResumeAsync()
                    )
                );
                var disposalTask = source.DisposeAsync().AsTask();
                disposal = disposalTask;
                Assert.Same(
                    failure,
                    await Assert.ThrowsAsync<InvalidOperationException>(() =>
                        disposalTask.WaitAsync(TestGuard)
                    )
                );
            }
            finally
            {
                if (disposal is null)
                    await source.DisposeAsync();
                else if (!disposal.IsCompleted)
                    await disposal;
            }
        }
        await using var restarted = ZLinkProcessExecutionPool.CreateReceiveSource();
        await restarted.ResumeAsync();
        Assert.False(Thread.CurrentThread.IsThreadPoolThread);
    }

    [Fact]
    public async Task ThrowingFailureReporterRemainsObservableAtSourceTermination()
    {
        var failure = new InvalidOperationException("Application turn failure.");
        var reportingFailure = new InvalidOperationException("Failure reporter failure.");
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var source = ZLinkProcessExecutionPool.CreateApplicationSource(
            _ =>
            {
                entered.TrySetResult();
                throw failure;
            },
            _ => throw reportingFailure
        );
        source.Signal();
        await entered.Task.WaitAsync(TestGuard);
        var observed = await Assert.ThrowsAnyAsync<Exception>(() =>
            source.DisposeAsync().AsTask().WaitAsync(TestGuard)
        );
        var aggregate = Assert.IsType<AggregateException>(observed);
        Assert.Contains(failure, aggregate.InnerExceptions);
        Assert.Contains(reportingFailure, aggregate.InnerExceptions);
    }

    internal sealed class OccupiedApplicationWorkers(Exception? expectedFailure = null)
        : IAsyncDisposable
    {
        private readonly CountdownEvent _entered = new(
            ZLinkProcessExecutionPool.ApplicationWorkerCount
        );
        private readonly ManualResetEventSlim _release = new();
        private readonly List<ZLinkProcessExecutionPool.ApplicationSource> _sources = [];
        private Task? _disposal;

        internal void Enter()
        {
            for (var index = 0; index < ZLinkProcessExecutionPool.ApplicationWorkerCount; index++)
            {
                var source = ZLinkProcessExecutionPool.CreateApplicationSource(
                    _ =>
                    {
                        _entered.Signal();
                        Assert.True(_release.Wait(TestGuard));
                        return ValueTask.CompletedTask;
                    },
                    _ => { }
                );
                _sources.Add(source);
                source.Signal();
            }
            Assert.True(_entered.Wait(TestGuard));
        }

        internal void Release() => _release.Set();

        public ValueTask DisposeAsync() => new(_disposal ??= DisposeCoreAsync());

        private async Task DisposeCoreAsync()
        {
            Release();
            foreach (var source in _sources)
                try
                {
                    await source.DisposeAsync().AsTask().WaitAsync(TestGuard);
                }
                catch (Exception failure) when (ReferenceEquals(failure, expectedFailure)) { }
            _entered.Dispose();
            _release.Dispose();
        }
    }

    private class DrainNode : DispatchProxy
    {
        private readonly Channel<bool> _entered = Channel.CreateUnbounded<bool>();
        private int _drainCount;
        internal int DrainCount => Volatile.Read(ref _drainCount);
        internal readonly ConcurrentDictionary<int, Thread> Threads = new();
        internal ValueTask<bool> Result = ValueTask.FromResult(false);
        internal Action<MeshReadyBatch>? Prepare;
        internal Func<MeshReadyDomains, MeshReadyDomains>? Ready { get; private set; }

        internal Task<bool> NextDrainAsync() => _entered.Reader.ReadAsync().AsTask();

        protected override object? Invoke(MethodInfo? method, object?[]? arguments)
        {
            switch (method!.Name)
            {
                case nameof(IMeshNode.SetReadyHandler):
                    Ready = (Func<MeshReadyDomains, MeshReadyDomains>)arguments![0]!;
                    return null;
                case nameof(IMeshNode.DrainReadyAsync):
                    Interlocked.Increment(ref _drainCount);
                    Threads.TryAdd(Environment.CurrentManagedThreadId, Thread.CurrentThread);
                    Prepare?.Invoke((MeshReadyBatch)arguments![1]!);
                    _entered.Writer.TryWrite(Thread.CurrentThread.IsThreadPoolThread);
                    return Result;
                case nameof(IMeshNode.CloseSubmissions):
                    return null;
                default:
                    throw new NotSupportedException(method.Name);
            }
        }
    }
}
