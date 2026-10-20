using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshReceiveWakeTests
{
    private static readonly TimeSpan CompletionBound = TimeSpan.FromSeconds(5);

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
        node._receiveWakeTimer = wake;
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
            held.Dispose();

            // Native Wait has no elapsed-time exit in this fixture. Only an
            // owner notification can resume it; the bound guards a stuck test.
            await poller.SecondWait.Task.WaitAsync(CompletionBound);
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

    private sealed class EmptyIngressRouter(IRouterSocket inner)
        : PressureSocket<IRouterSocket>(inner),
            IRouterSocket
    {
        internal IRouterSocket Inner => inner;
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

    private sealed class ControlledReceivePoller : IPoller
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
        internal readonly TaskCompletionSource SecondWait = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        internal int OwnerThread { get; private set; }
        internal int ResumeThread { get; private set; }
        internal bool FailRearm { get; init; }
        internal bool RespectTimeoutAfterWake { get; init; }

        internal ControlledReceivePoller() => _inner.Add(_control, ControlSlot);

        internal void AllowWait() => _allowWait.Set();

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
            if (++_waitCount == 1)
            {
                OwnerThread = Environment.CurrentManagedThreadId;
                FirstWait.TrySetResult();
                _allowWait.Wait();
            }
            else
                SecondWait.TrySetResult();
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
        }

        public int Size => _inner.Size;

        public void Add(IZlinkSocket socket, PollEventFlags events, nuint slot) =>
            _inner.Add(socket is EmptyIngressRouter adapter ? adapter.Inner : socket, events, slot);

        public void Add(ISocketMonitor monitor, PollEventFlags events, nuint slot) =>
            _inner.Add(monitor, events, slot);

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
