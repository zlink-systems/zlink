using System.Reflection;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshReceiveWakeTests
{
    private static readonly TimeSpan CompletionBound = TimeSpan.FromSeconds(5);

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
        var stop = Field<CancellationTokenSource>(node, "_stop");
        try
        {
            await poller.FirstWait.Task.WaitAsync(CompletionBound);
            // The owner is held before entering native Wait. Use the existing
            // ingress seam to exhaust admission without modifying an active poller.
            typeof(ZLinkManagedMeshNode)
                .GetMethod("DrainRawSocket", BindingFlags.Instance | BindingFlags.NonPublic)!
                .Invoke(node, [stop.Token]);
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
            stop.Cancel();
            poller.ReleaseWait();
            await node.DisposeAsync();
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
        var stop = Field<CancellationTokenSource>(node, "_stop");
        try
        {
            await poller.FirstWait.Task.WaitAsync(CompletionBound);
            var handoff = (Task)
                typeof(ZLinkManagedMeshNode)
                    .GetMethod(
                        "WaitForRawApplicationAdmissionAsync",
                        BindingFlags.Instance | BindingFlags.NonPublic
                    )!
                    .Invoke(node, [queue, stop.Token])!;
            held.Dispose();
            await handoff.WaitAsync(CompletionBound);
            Assert.Equal(1UL, queue.GetStatus().ReservedSupplyPermits);

            var stopping = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            using var registration = stop.Token.Register(() => stopping.TrySetResult());
            var shutdown = node.DisposeAsync();
            await stopping.Task.WaitAsync(CompletionBound);
            Assert.Equal(1UL, queue.GetStatus().ReservedSupplyPermits);
            poller.AllowWait();
            await shutdown.AsTask().WaitAsync(CompletionBound);
            Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
            Assert.Equal(0UL, queue.GetStatus().CapacityWaiters);
            Assert.Null(
                Field<ZLinkApplicationJobQueueLease?>(node, "_reservedRawApplicationAdmission")
            );
        }
        finally
        {
            if (!node.DisposeAsync().IsCompletedSuccessfully)
                poller.ReleaseWait();
            await node.DisposeAsync();
        }
    }

    private static T Field<T>(ZLinkManagedMeshNode node, string name) =>
        (T)
            typeof(ZLinkManagedMeshNode)
                .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(node)!;

    private sealed class ControlledReceivePoller : IPoller
    {
        private static readonly nuint ControlSlot = nuint.MaxValue;
        private readonly IPoller _inner = Systems.Zlink.Zlink.CreatePoller();
        private readonly IZlinkTimer _control = Systems.Zlink.Zlink.CreateTimer();
        private readonly ManualResetEventSlim _allowWait = new(false);
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

        internal ControlledReceivePoller() => _inner.Add(_control, ControlSlot);

        internal void AllowWait() => _allowWait.Set();

        internal void ReleaseWait()
        {
            AllowWait();
            _control.Start(TimeSpan.FromTicks(1), 1);
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
            var count = _inner.Wait(destination, Timeout.InfiniteTimeSpan);
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
            _inner.Modify(socket, events);
        }

        public int Size => _inner.Size;

        public void Add(IZlinkSocket socket, PollEventFlags events, nuint slot) =>
            _inner.Add(socket, events, slot);

        public void Add(ISocketMonitor monitor, PollEventFlags events, nuint slot) =>
            _inner.Add(monitor, events, slot);

        public void Add(IZlinkTimer timer, nuint slot) => _inner.Add(timer, slot);

        public void AddFd(int fd, PollEventFlags events, nuint slot) =>
            _inner.AddFd(fd, events, slot);

        public void Modify(ISocketMonitor monitor, PollEventFlags events) =>
            _inner.Modify(monitor, events);

        public void ModifyFd(int fd, PollEventFlags events) => _inner.ModifyFd(fd, events);

        public bool Remove(IZlinkSocket socket) => _inner.Remove(socket);

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
            _inner.Remove(_control);
            _control.Dispose();
            _inner.Dispose();
            _allowWait.Dispose();
        }
    }
}
