using System.Collections.Concurrent;
using Systems.Zlink.Framework.Runtime.Protocol;
using Xunit.Abstractions;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshReceiveWakeTests(ITestOutputHelper output)
{
    private static readonly TimeSpan CompletionBound = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task IdleReceiveOwnerWaitsForReadinessWithoutAPeriodicDeadline()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var poller = new ControlledReceivePoller { CaptureIdleWait = true };
        await using var node = new ZLinkManagedMeshNode(
            context,
            "idle-wait",
            receivePollerFactory: () => poller
        );
        node.SetRoutingId(RoutingId.From("idle-wait"));
        node.Start();
        Assert.Equal(
            Timeout.InfiniteTimeSpan,
            await poller.IdleWait.Task.WaitAsync(CompletionBound)
        );
        Assert.True(poller.MonitorRegistered);
    }

    [Fact]
    public async Task LongManagedDeadlineWakesAnOtherwiseUnboundedWait()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "long-deadline-wake");
        var wake = new CountingWakeTimer();
        node.ReceiveWakeObserver = () => wake.Starts++;
        Assert.True(
            node.TryBeginOperation(MeshOperationKind.NodeRequest, TimeSpan.FromSeconds(30), out _)
        );
        Assert.Equal(1, wake.Starts);
    }

    [Theory]
    [InlineData(1, 20)]
    [InlineData(100, 20)]
    [InlineData(1, 30)]
    [InlineData(100, 30)]
    public async Task ManagedDeadlineWakesOnlyWhenItAdvancesTheNextWait(
        int additionalRequests,
        int timeoutMilliseconds
    )
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            "deadline-wake-count",
            deadlineTimeProvider: new ControllableTimeProvider()
        );
        var wake = new CountingWakeTimer();
        // No receive loop runs here: registration and the actual wake timer
        // starts are deterministic, including equal deadlines in the same slot.
        node.ReceiveWakeObserver = () => wake.Starts++;
        Register(TimeSpan.FromMilliseconds(20));
        Assert.Equal(1, wake.Starts);
        wake.Starts = 0;
        for (var index = 0; index < additionalRequests; index++)
            Register(TimeSpan.FromMilliseconds(timeoutMilliseconds));
        Assert.Equal(0, wake.Starts);
        Register(TimeSpan.FromMilliseconds(10));
        Assert.Equal(1, wake.Starts);

        void Register(TimeSpan timeout) =>
            Assert.True(node.TryBeginOperation(MeshOperationKind.SpotRequest, timeout, out _));
    }

    [Fact]
    public async Task NewManagedDeadlineWakesAnIdleReceiveOwner()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        using var queue = new ZLinkApplicationJobQueue(
            new(ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1)
        );
        using var held = await queue.AcquireAsync(
            CancellationToken.None,
            ZLinkApplicationJobOrigin.Remote
        );
        var poller = new ControlledReceivePoller { RespectTimeoutAfterWake = true };
        var node = new ZLinkManagedMeshNode(
            context,
            "deadline-wake",
            applicationJobQueue: queue,
            receivePollerFactory: () => poller
        );
        node.SetRoutingId(RoutingId.From("deadline-wake"));
        node.Start();
        try
        {
            await poller.FirstWait.Task.WaitAsync(CompletionBound);
            Assert.True(
                node.TryBeginOperation(
                    MeshOperationKind.NodeRequest,
                    TimeSpan.FromMilliseconds(10),
                    out var operation
                )
            );
            var token = operation.Token;
            var expired = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            using var registration = token.Register(() => expired.TrySetResult());
            poller.AllowWait();
            // The first wait cannot time out. A deadline registration must wake
            // the owner, which then uses the 10 ms bound on its next rotation.
            await expired.Task.WaitAsync(CompletionBound);
            Assert.True(poller.SecondWait.Task.IsCompleted);
            Assert.Equal(0UL, queue.GetStatus().CapacityWaiters);
        }
        finally
        {
            var shutdown = node.DisposeAsync();
            poller.ReleaseWait();
            await shutdown;
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task PermitGrantResumesReceiveOwnerWithoutAPollTick(bool failRearm)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        using var queue = new ZLinkApplicationJobQueue(
            new(ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1)
        );
        using var held = await queue.AcquireAsync(
            CancellationToken.None,
            ZLinkApplicationJobOrigin.Remote
        );
        var poller = new ControlledReceivePoller { FailRearm = failRearm };
        var node = new ZLinkManagedMeshNode(
            context,
            "receive-wake",
            applicationJobQueue: queue,
            receivePollerFactory: () => poller
        );
        node.SetRoutingId(RoutingId.From("receive-wake"));
        node.Start();
        var stop = node.ReceiveStopToken;
        try
        {
            await poller.FirstWait.Task.WaitAsync(CompletionBound);
            // The owner is held before entering native Wait. Use the existing
            // ingress seam to exhaust admission without modifying an active poller.
            node.DrainRawSocket(stop);
            Assert.Equal(1UL, queue.GetStatus().CapacityWaiters);
            poller.AllowWait();
            await poller.SourcePaused.Task.WaitAsync(CompletionBound);
            held.Dispose();

            // Native Wait has no elapsed-time exit in this fixture. Only an
            // owner notification can resume it; the bound guards a stuck test.
            await poller.RearmObserved.Task.WaitAsync(CompletionBound);
            Assert.Equal(poller.OwnerThread, poller.ResumeThread);
            Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
            Assert.Equal(0UL, queue.GetStatus().CapacityWaiters);
        }
        finally
        {
            var shutdown = node.DisposeAsync();
            poller.ReleaseWait();
            await shutdown;
        }
    }

    [Fact]
    public async Task ShutdownWakesOwnerAndReturnsPublishedGrant()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        using var queue = new ZLinkApplicationJobQueue(
            new(ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1)
        );
        using var held = await queue.AcquireAsync(
            CancellationToken.None,
            ZLinkApplicationJobOrigin.Remote
        );
        var poller = new ControlledReceivePoller();
        var node = new ZLinkManagedMeshNode(
            context,
            "shutdown-wake",
            applicationJobQueue: queue,
            receivePollerFactory: () => poller
        );
        node.SetRoutingId(RoutingId.From("shutdown-wake"));
        node.Start();
        var stop = node.ReceiveStopToken;
        try
        {
            await poller.FirstWait.Task.WaitAsync(CompletionBound);
            var handoff = node.WaitForRawApplicationAdmissionAsync(queue, stop);
            held.Dispose();
            await handoff.WaitAsync(CompletionBound);
            Assert.Equal(1UL, queue.GetStatus().ReservedSupplyPermits);

            var stopping = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            using var registration = stop.Register(() => stopping.TrySetResult());
            var shutdown = node.DisposeAsync();
            await stopping.Task.WaitAsync(CompletionBound);
            Assert.Equal(1UL, queue.GetStatus().ReservedSupplyPermits);
            poller.AllowWait();
            await shutdown.AsTask().WaitAsync(CompletionBound);
            Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
            Assert.Equal(0UL, queue.GetStatus().CapacityWaiters);
            Assert.False(node.HasReservedRawApplicationAdmission);
        }
        finally
        {
            if (!node.DisposeAsync().IsCompletedSuccessfully)
                poller.ReleaseWait();
            await node.DisposeAsync();
        }
    }

    [Fact]
    public async Task RawIngressAddsNoAllocationToTheReceivedContainer()
    {
        await using var inner = Systems.Zlink.Zlink.CreateContext();
        var context = new EmptyIngressContext(inner);
        var poller = new ControlledReceivePoller();
        var node = new ZLinkManagedMeshNode(
            context,
            "ingress-allocation",
            receivePollerFactory: () => poller
        );
        node.SetRoutingId(RoutingId.From("ingress-allocation"));
        node.Start();
        try
        {
            await poller.FirstWait.Task.WaitAsync(CompletionBound);
            var router = Assert.IsType<EmptyIngressRouter>(context.Router);
            // The real receive owner is held before Wait. Its existing internal
            // drain seam runs on this thread against a public socket adapter.
            for (var index = 0; index < 128; index++)
            {
                router.Remaining = 1;
                do node.DrainRawSocket(CancellationToken.None);
                while (router.Remaining != 0);
            }
            const int iterations = 1000;
            var calls = router.Calls;
            var start = GC.GetAllocatedBytesForCurrentThread();
            for (var index = 0; index < iterations; index++)
            {
                router.Remaining = 1;
                do node.DrainRawSocket(CancellationToken.None);
                while (router.Remaining != 0);
            }
            var actual = GC.GetAllocatedBytesForCurrentThread() - start;
            Assert.Equal(iterations, router.Accepted - 128);
            Assert.True(router.Calls - calls >= iterations);
            // Match the number of receipt containers the bounded drain really
            // attempted; no platform-specific object size is embedded here.
            start = GC.GetAllocatedBytesForCurrentThread();
            for (var index = 0; index < router.Calls - calls; index++)
            {
                using var receipt = Received.Create();
                if (index < iterations)
                    _ = receipt.Parts;
                _ = receipt.RoutingId;
            }
            var baseline = GC.GetAllocatedBytesForCurrentThread() - start;
            Assert.True(
                actual == baseline,
                $"ingress={actual}B, receipt baseline={baseline}B, excess={actual - baseline}B/{iterations}"
            );
        }
        finally
        {
            var shutdown = node.DisposeAsync();
            poller.ReleaseWait();
            await shutdown;
        }
    }

    // Spec 08 §4.2: a receive rotation runs its management work once. With no
    // peer activity due, that work is the two uncontended node turns that read
    // liveness and operation deadlines; it creates no empty result container.
    [Fact]
    public async Task IdleReceiveRotationManagementAddsNoAllocationBeyondItsNodeTurns()
    {
        const int warmup = 128;
        const int iterations = 1000;
        var poller = new ControlledReceivePoller();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var node = new ZLinkManagedMeshNode(
            context,
            "idle-rotation-allocation",
            receivePollerFactory: () => poller
        );
        node.SetRoutingId(RoutingId.From("idle-rotation-allocation"));
        await using var lane = new ZLinkStateLane();
        long actual = 0;
        long baseline = 0;
        var rotations = 0;
        long last = 0;
        SemaphoreSlim rotated = new(0);
        // The window opens after the controlled poller returns and closes before
        // its next wait, so it holds only the node's own rotation.
        poller.AfterWait = () => last = GC.GetAllocatedBytesForCurrentThread();
        poller.BeforeWait = () =>
        {
            var now = GC.GetAllocatedBytesForCurrentThread();
            var measured = now - last;
            var start = GC.GetAllocatedBytesForCurrentThread();
            _ = lane.RunAsync(static value => value, 0).GetAwaiter().GetResult();
            _ = lane.RunAsync(static value => value, 0).GetAwaiter().GetResult();
            var control = GC.GetAllocatedBytesForCurrentThread() - start;
            if (rotations > warmup)
            {
                actual += measured;
                baseline += control;
            }
            rotations++;
            rotated.Release();
        };
        node.Start();
        try
        {
            await poller.FirstWait.Task.WaitAsync(CompletionBound);
            poller.AllowWait();
            await rotated.WaitAsync(CompletionBound);
            for (var index = 0; index < warmup + iterations; index++)
            {
                node.SignalReceiveOwner();
                Assert.True(await rotated.WaitAsync(CompletionBound));
            }
            output.WriteLine(
                $"idle rotation={actual}B, node turns={baseline}B, excess={actual - baseline}B/{iterations}"
            );
            Assert.True(
                actual == baseline,
                $"idle rotation={actual}B, node turns={baseline}B, excess={actual - baseline}B/{iterations}"
            );
        }
        finally
        {
            var shutdown = node.DisposeAsync();
            poller.ReleaseWait();
            await shutdown;
        }
    }

    [Fact]
    public async Task NodeIngressSourceGenerationAddsNoAllocationBeyondItsOwners()
    {
        const int warmup = 128;
        const int iterations = 1000;
        await using var inner = Systems.Zlink.Zlink.CreateContext();
        var context = new EmptyIngressContext(inner);
        var targetPoller = new ControlledReceivePoller { RespectTimeoutAfterWake = true };
        var sourcePoller = targetPoller;
        await using var source = new ZLinkManagedMeshNode(
            inner,
            "source-allocation",
            receivePollerFactory: () => sourcePoller
        );
        var target = new ZLinkManagedMeshNode(
            context,
            "source-allocation",
            receivePollerFactory: () => targetPoller
        );
        var sourceRid = RoutingId.From("allocation-source");
        var targetRid = RoutingId.From("allocation-target");
        var endpoint = $"inproc://source-allocation-{Guid.NewGuid():N}";
        source.SetRoutingId(sourceRid);
        target.SetRoutingId(targetRid);
        target.SetBind(endpoint);
        source.SetBind($"inproc://source-origin-{Guid.NewGuid():N}");
        source.ConnectPeer(endpoint, targetRid);
        var sourceAdmitted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var targetAdmitted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        targetPoller.BeforeWait = () =>
        {
            if (source.Status().AdmittedPeerCount == 1)
                sourceAdmitted.TrySetResult();
            if (target.Status().AdmittedPeerCount == 1)
                targetAdmitted.TrySetResult();
        };
        await using var lane = new ZLinkStateLane();
        ZLinkMeshNodeOwnedMailbox? baselineMailbox = null;
        try
        {
            target.Start();
            source.Start();
            var router = Assert.IsType<EmptyIngressRouter>(context.Router);
            router.ReceiveRealRecords = true;
            targetPoller.AllowWait();
            sourcePoller.AllowWait();
            try
            {
                await Task.WhenAll(sourceAdmitted.Task, targetAdmitted.Task)
                    .WaitAsync(CompletionBound);
            }
            catch (TimeoutException)
            {
                throw new InvalidOperationException(
                    $"Admission did not finish: source={source.Status().AdmittedPeerCount}, target={target.Status().AdmittedPeerCount}, real={router.ReceiveRealRecords}."
                );
            }
            var ready = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            long actual = 0;
            long baseline = 0;
            long lastActual = 0;
            long lastBaseline = 0;
            var samples = 0;
            var key = MeshMailboxKey.ForNode(MeshReadyDomains.Application);
            var readyMap = new ConcurrentDictionary<MeshMailboxKey, bool>();
            // The baseline retains the one uncontended source lane turn that reads
            // route currency and generation together, and the existing
            // record/mailbox containers without fixed byte sizes.
            var mailbox = baselineMailbox = new ZLinkMeshNodeOwnedMailbox(
                static _ => { },
                static _ => { },
                () => readyMap.TryAdd(key, true)
            );
            target.SetReadyHandler(domains =>
            {
                // Binding has already materialized the receipt's lazy public
                // properties; both counters run on this same receive thread.
                lastActual = GC.GetAllocatedBytesForCurrentThread() - router.RecordStart;
                var start = GC.GetAllocatedBytesForCurrentThread();
                _ = lane.RunAsync(static value => value, 0).GetAwaiter().GetResult();
                if (
                    !mailbox.TryEnqueue(
                        new ZLinkMeshQueuedRecord(default, Array.Empty<Message>(), 0)
                    )
                )
                    throw new InvalidOperationException("Baseline mailbox refused the record.");
                lastBaseline = GC.GetAllocatedBytesForCurrentThread() - start;
                samples++;
                ready.TrySetResult();
                return domains;
            });
            using var payload = Message.From(new byte[] { 1 });
            using var readyBatch = new MeshReadyBatch();
            using var received = new MeshReceiveBatch();
            using var baselineReceived = new MeshReceiveBatch();
            for (var index = 0; index < warmup + iterations; index++)
            {
                Assert.Equal(SubmitResult.Ok, source.SendToNode(targetRid, [payload]));
                await ready.Task.WaitAsync(CompletionBound);
                if (index >= warmup)
                {
                    actual += lastActual;
                    baseline += lastBaseline;
                }
                Assert.False(
                    target.DrainReady(MeshReadyDomains.Application, readyBatch, RecvFlags.DontWait)
                );
                Assert.Equal(1, readyBatch.Count);
                var claim = readyBatch.TakeClaim(0);
                Assert.True(claim.Receive(received, RecvFlags.DontWait));
                Assert.Equal(1, received.Count);
                Assert.Equal(MeshRecordKind.NodeSend, received[0].Kind);
                Assert.Equal(sourceRid, received[0].SourceNodeRid);
                Assert.True(received[0].SourceBindingGeneration != 0);
                received.Reset();
                readyBatch.Reset();
                Assert.True(await mailbox.ClaimAndDrainAsync(baselineReceived, 1));
                baselineReceived.Reset();
                mailbox.Release();
                Assert.True(readyMap.TryRemove(key, out _));
                ready = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
            }
            Assert.Equal(warmup + iterations, samples);
            output.WriteLine(
                $"node ingress={actual}B, lane + owner baseline={baseline}B, excess={actual - baseline}B/{iterations}"
            );
            Assert.True(
                actual == baseline,
                $"node ingress={actual}B, lane + owner baseline={baseline}B, excess={actual - baseline}B/{iterations}"
            );
        }
        finally
        {
            try
            {
                var shutdown = target.DisposeAsync();
                targetPoller.ReleaseWait();
                sourcePoller.ReleaseWait();
                await shutdown;
            }
            finally
            {
                baselineMailbox?.Dispose();
            }
        }
    }

    [Fact]
    public async Task PoolWaitFailureTerminatesNodeAndJoinsTheReceiveWorker()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var poller = new ControlledReceivePoller();
        var failure = new InvalidOperationException("Injected process wait failure.");
        Thread? receiver = null;
        var failedWait = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        poller.BeforeWait = () =>
        {
            receiver = Thread.CurrentThread;
            failedWait.TrySetResult();
            throw failure;
        };
        var node = new ZLinkManagedMeshNode(
            context,
            "wait-failure",
            receivePollerFactory: () => poller
        );
        node.SetRoutingId(RoutingId.From("wait-failure"));
        node.Start();
        await poller.FirstWait.Task.WaitAsync(CompletionBound);
        poller.AllowWait();
        await failedWait.Task.WaitAsync(CompletionBound);
        var observed = await Assert.ThrowsAnyAsync<Exception>(() =>
            node.DisposeAsync().AsTask().WaitAsync(CompletionBound)
        );
        var failures = observed is AggregateException aggregate
            ? aggregate.Flatten().InnerExceptions
            : new[] { observed }.AsEnumerable();
        Assert.Contains(failures, error => ReferenceEquals(error, failure));
        Assert.NotNull(receiver);
        Assert.False(receiver!.IsAlive);
        await using var restarted = ZLinkProcessExecutionPool.CreateReceiveSource();
        await restarted.ResumeAsync();
        Assert.False(Thread.CurrentThread.IsThreadPoolThread);
    }

    private sealed class EmptyIngressRouter(IRouterSocket inner)
        : PressureSocket<IRouterSocket>(inner),
            IRouterSocket
    {
        internal IRouterSocket Inner => inner;
        internal bool ReceiveRealRecords;
        internal long RecordStart;
        internal int Remaining;
        internal int Calls;
        internal int Accepted;
        public new RouterSocketOptions Options => inner.Options;

        public void SetRoutingId(RoutingId rid) => inner.SetRoutingId(rid);

        public RoutingId GetRoutingId() => inner.GetRoutingId();

        public SendOperation Send(RoutingId rid) => inner.Send(rid);

        public RequestOperation Request(RoutingId rid) => inner.Request(rid);

        public ReplyOperation Reply(RoutingId rid, ReplyToken token) => inner.Reply(rid, token);

        public IReadOnlyList<RouterRoute> RoutesSnapshot() => inner.RoutesSnapshot();

        public new bool Recv(Received result, RecvFlags flags = RecvFlags.None)
        {
            if (ReceiveRealRecords)
            {
                var accepted = inner.Recv(result, flags);
                if (accepted && result.Parts.Count != 0)
                {
                    var head = result.Parts[0].AsReadOnlySpan();
                    if (head.Length >= 5 && head[3] == (byte)ServiceWireConstants.Command.NodeSend)
                    {
                        _ = result.RoutingId;
                        RecordStart = GC.GetAllocatedBytesForCurrentThread();
                    }
                }
                return accepted;
            }
            Calls++;
            if (Remaining == 0)
                return false;
            Remaining--;
            Accepted++;
            // An empty receipt exercises rejection and automatic ownership return.
            return true;
        }
    }

    private sealed class EmptyIngressContext(IContext inner) : IContext
    {
        internal EmptyIngressRouter? Router;
        public IContextOptions Options => inner.Options;

        public IRouterSocket CreateRouterSocket() => Router = new(inner.CreateRouterSocket());

        public IPairSocket CreatePairSocket() => inner.CreatePairSocket();

        public IDealerSocket CreateDealerSocket() => inner.CreateDealerSocket();

        public IPubSocket CreatePubSocket() => inner.CreatePubSocket();

        public ISubSocket CreateSubSocket() => inner.CreateSubSocket();

        public IXPubSocket CreateXPubSocket() => inner.CreateXPubSocket();

        public IXSubSocket CreateXSubSocket() => inner.CreateXSubSocket();

        public IStreamSocket CreateStreamSocket() => inner.CreateStreamSocket();

        public void Shutdown() => inner.Shutdown();

        public void RecalculateAutoHwm() => inner.RecalculateAutoHwm();

        public CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot() => inner.GetCoreHwmBudgetSnapshot();

        public void ResetCoreHwmBudgetMetrics() => inner.ResetCoreHwmBudgetMetrics();

        public void Dispose() => inner.Dispose();

        public ValueTask DisposeAsync() => inner.DisposeAsync();
    }

    private sealed class CountingWakeTimer : IZlinkTimer
    {
        internal int Starts { get; set; }

        public void Start(TimeSpan interval, ulong repeatCount) => Starts++;

        public void Stop() { }

        public ulong? Recv(RecvFlags flags = RecvFlags.None) => null;

        public void Close() { }

        public void Dispose() { }

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    internal sealed class ControlledReceivePoller : IPoller
    {
        private static readonly nuint ControlSlot = nuint.MaxValue;
        private readonly IPoller _inner = Systems.Zlink.Zlink.CreatePoller();
        private readonly IZlinkTimer _control = Systems.Zlink.Zlink.CreateTimer();
        private readonly ManualResetEventSlim _allowWait = new(false);
        private readonly object _lifetime = new();
        private bool _disposed;
        private int _waitCount;

        internal readonly TaskCompletionSource FirstWait = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        internal readonly TaskCompletionSource RearmObserved = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        internal readonly TaskCompletionSource SourcePaused = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        internal readonly TaskCompletionSource SecondWait = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        internal Action? BeforeWait;
        internal Action? AfterWait;
        internal int OwnerThread { get; private set; }
        internal int ResumeThread { get; private set; }
        internal bool FailRearm { get; init; }
        internal bool RespectTimeoutAfterWake { get; init; }
        internal bool CaptureIdleWait { get; init; }
        internal bool MonitorRegistered { get; private set; }
        internal readonly TaskCompletionSource<TimeSpan> IdleWait = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        internal ControlledReceivePoller() => _inner.Add(_control, ControlSlot);

        internal void AllowWait() => _allowWait.Set();

        // One controlled wake: the owner runs a rotation with no ready source.
        internal void Tick() => _control.Start(TimeSpan.FromTicks(1), 1);

        // Node disposal can wake the owner, which then disposes this poller before
        // the test's fallback release runs. A disposed poller has no Wait to release.
        internal void ReleaseWait()
        {
            lock (_lifetime)
            {
                if (_disposed)
                    return;
                AllowWait();
                _control.Start(TimeSpan.FromTicks(1), 1);
            }
        }

        public int Wait(Span<PollEvent> destination, TimeSpan timeout)
        {
            if (CaptureIdleWait)
            {
                if (++_waitCount == 1)
                    return 0;
                IdleWait.TrySetResult(timeout);
                return _inner.Wait(destination, Timeout.InfiniteTimeSpan);
            }
            if (++_waitCount == 1)
            {
                OwnerThread = Environment.CurrentManagedThreadId;
                FirstWait.TrySetResult();
                _allowWait.Wait();
            }
            else
                SecondWait.TrySetResult();
            if (ResumeThread != 0)
                RearmObserved.TrySetResult();
            BeforeWait?.Invoke();
            var count = _inner.Wait(
                destination,
                RespectTimeoutAfterWake && _waitCount > 1 ? timeout : Timeout.InfiniteTimeSpan
            );
            var written = 0;
            for (var index = 0; index < count; index++)
            {
                var ready = destination[index];
                if (ready.SourceKind == PollSourceKind.Timer && ready.Slot == ControlSlot)
                    _control.Recv();
                else
                    destination[written++] = ready;
            }
            AfterWait?.Invoke();
            return written;
        }

        public void Modify(IZlinkSocket socket, PollEventFlags events)
        {
            if ((events & PollEventFlags.PollIn) != 0)
            {
                ResumeThread = Environment.CurrentManagedThreadId;
                if (FailRearm)
                    throw new InvalidOperationException("Controlled receive rearm failure.");
            }
            _inner.Modify(socket is EmptyIngressRouter adapter ? adapter.Inner : socket, events);
            if ((events & PollEventFlags.PollIn) == 0)
                SourcePaused.TrySetResult();
        }

        public int Size => _inner.Size;

        public void Add(IZlinkSocket socket, PollEventFlags events, nuint slot)
        {
            _inner.Add(socket is EmptyIngressRouter adapter ? adapter.Inner : socket, events, slot);
            if ((events & PollEventFlags.PollIn) == 0)
                SourcePaused.TrySetResult();
        }

        public void Add(ISocketMonitor monitor, PollEventFlags events, nuint slot)
        {
            MonitorRegistered = true;
            _inner.Add(monitor, events, slot);
        }

        public void Add(IZlinkTimer timer, nuint slot) => _inner.Add(timer, slot);

        public void AddFd(int fd, PollEventFlags events, nuint slot) =>
            _inner.AddFd(fd, events, slot);

        public void Modify(ISocketMonitor monitor, PollEventFlags events) =>
            _inner.Modify(monitor, events);

        public void ModifyFd(int fd, PollEventFlags events) => _inner.ModifyFd(fd, events);

        public bool Remove(IZlinkSocket socket) =>
            _inner.Remove(socket is EmptyIngressRouter adapter ? adapter.Inner : socket);

        public bool Remove(ISocketMonitor monitor) => _inner.Remove(monitor);

        public bool Remove(IZlinkTimer timer) => _inner.Remove(timer);

        public bool Remove(int fd) => _inner.Remove(fd);

        public void Clear() => _inner.Clear();

        public void Close() => Dispose();

        public ValueTask DisposeAsync()
        {
            Dispose();
            return ValueTask.CompletedTask;
        }

        public void Dispose()
        {
            lock (_lifetime)
            {
                if (_disposed)
                    return;
                _disposed = true;
                _inner.Remove(_control);
                _control.Dispose();
                _inner.Dispose();
                _allowWait.Dispose();
            }
        }
    }
}
