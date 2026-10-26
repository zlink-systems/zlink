using System.Diagnostics;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed partial class StatefulServiceRuntimeTests
{
    [Fact]
    public async Task LivenessDisconnect_NonNotFoundBindingFailureIsExposed()
    {
        await using var ownerContext = Systems.Zlink.Zlink.CreateContext();
        await using var peerContext = Systems.Zlink.Zlink.CreateContext();
        var bindingFailure = new ZlinkConnectException(
            ZlinkConnectException.ErrorCode.InternalError
        );
        var captured = new CapturingMeshSocketContext(ownerContext)
        {
            DisconnectFailure = bindingFailure,
        };
        try
        {
            using var poller = new DeferredMeshTurnPoller();
            await using var owner = new ZLinkManagedMeshNode(
                captured,
                "mesh",
                receivePollerFactory: () => poller
            );
            owner.SetRoutingId(RoutingId.From("disconnect-error-owner"));
            await using var peer = NewNode(peerContext, "disconnect-error-peer");
            owner.SetBind("tcp://127.0.0.1:0");
            owner.Start();
            peer.Start();
            peer.ConnectPeer(Assert.IsType<string>(owner.Status().LocalEndpoint), owner.RoutingId);
            await ObserveAsync(() => owner.Status().AdmittedPeerCount == 1);
            var error = Assert.Throws<ZlinkConnectException>(() =>
                owner.DisconnectPeer(peer.RoutingId)
            );
            Assert.Equal(bindingFailure.Result, error.Result);
            Assert.Same(bindingFailure, error);
            Assert.Single(owner.Peers(), candidate => candidate.RoutingId == peer.RoutingId);
            Assert.Equal(1, captured.DisconnectRidCalls);
        }
        finally
        {
            captured.DisposeCapturedRouter();
            ownerContext.Shutdown();
            peerContext.Shutdown();
        }
    }

    [Fact]
    public async Task LivenessDisconnect_AlreadyMissingPhysicalPeerKeepsReceiveOwnerRunning()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var captured = new CapturingMeshSocketContext(context);
        using var poller = new DeferredMeshTurnPoller();
        await using var owner = new ZLinkManagedMeshNode(
            captured,
            "mesh",
            receivePollerFactory: () => poller
        );
        await using var peer = NewNode(context, "liveness-missing-peer");
        owner.SetRoutingId(RoutingId.From("liveness-missing-owner"));
        owner.SetBind("tcp://127.0.0.1:0");
        owner.Start();
        peer.Start();
        peer.ConnectPeer(Assert.IsType<string>(owner.Status().LocalEndpoint), owner.RoutingId);
        await ObserveAsync(() =>
            owner.Status().AdmittedPeerCount == 1 && peer.Status().AdmittedPeerCount == 1
        );
        Assert.Equal(
            SubmitResult.Ok,
            owner.SendToNode(peer.RoutingId, [Message.From("before-disconnect"u8)])
        );
        await ObserveAsync(() => peer.Status().PendingApplicationMessages != 0);
        poller.Arm();
        owner.SignalReceiveOwner();
        Task? peerStop = null;
        try
        {
            await poller.Held.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(poller.HeldRouteEvent);
            captured.Router!.DisconnectRid(peer.RoutingId);
            // Stop the peer's connection intent without waiting for source removal.
            peer.DisconnectPeer(owner.RoutingId);
            await ObserveAsync(() =>
            {
                try
                {
                    captured.Router.DisconnectRid(peer.RoutingId);
                    return false;
                }
                catch (ZlinkConnectException error)
                    when (error.Result == ZlinkConnectException.ErrorCode.NotFound)
                {
                    return true;
                }
            });
            // Removal also uses the shared receive role. Join after releasing it.
            peerStop = peer.DisposeAsync().AsTask();
            Assert.Equal(1U, owner.Status().AdmittedPeerCount);

            // A native timer advances beyond the admitted peer's liveness deadline.
            // The held batch predates physical teardown and contains no route event.
            using var deadline = Systems.Zlink.Zlink.CreateTimer();
            using var deadlinePoller = Systems.Zlink.Zlink.CreatePoller();
            deadlinePoller.Add(deadline, 1);
            deadline.Start(ZLinkServiceLiveness.PeerTimeout, 1);
            await Task.Run(() =>
            {
                var events = new PollEvent[1];
                Assert.Equal(1, deadlinePoller.Wait(events, Timeout.InfiniteTimeSpan));
                Assert.Equal(PollSourceKind.Timer, events[0].SourceKind);
            });
            var stillMissing = Assert.Throws<ZlinkConnectException>(() =>
                captured.Router.DisconnectRid(peer.RoutingId)
            );
            Assert.Equal(ZlinkConnectException.ErrorCode.NotFound, stillMissing.Result);
            poller.Release();
            await poller.TurnCompleted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            // The next native Wait has not run: route observation cannot cause this.
            Assert.Equal(0U, owner.Status().AdmittedPeerCount);
            Assert.NotEqual(MeshNodeState.Error, owner.Status().State);
            Assert.DoesNotContain(
                owner.Peers(),
                candidate => candidate.RoutingId == peer.RoutingId
            );
        }
        finally
        {
            poller.ReleaseAll();
            if (peerStop is not null)
                await peerStop;
        }

        await using var sender = NewNode(context, "liveness-after-cleanup");
        sender.Start();
        sender.ConnectPeer(Assert.IsType<string>(owner.Status().LocalEndpoint), owner.RoutingId);
        await ObserveAsync(() => sender.Status().AdmittedPeerCount == 1);
        Assert.Equal(
            SubmitResult.Ok,
            sender.SendToNode(owner.RoutingId, [Message.From("after-cleanup"u8)])
        );
        await ObserveAsync(() => owner.Status().PendingApplicationMessages != 0);
        using var ready = new MeshReadyBatch();
        owner.DrainReady(MeshReadyDomains.All, ready, RecvFlags.DontWait);
        Assert.True(ready.Count > 0);
        var delivered = false;
        for (var index = 0; index < ready.Count; index++)
        {
            using var claim = ready.TakeClaim(index);
            using var received = new MeshReceiveBatch();
            while (claim.Receive(received, RecvFlags.DontWait))
            {
                for (var record = 0; record < received.Count; record++)
                {
                    foreach (var part in received.RetainMessage(record))
                    {
                        delivered |= part.AsReadOnlySpan().SequenceEqual("after-cleanup"u8);
                        part.Dispose();
                    }
                }
                received.Reset();
            }
        }
        Assert.True(delivered);
    }

    private sealed class DeferredMeshTurnPoller : IPoller
    {
        private readonly IPoller _inner = Systems.Zlink.Zlink.CreatePoller();
        private readonly ManualResetEventSlim _release = new(false);
        private readonly ManualResetEventSlim _continue = new(false);
        private PollEvent[] _deferred = [];
        private int _phase;
        public TaskCompletionSource Held { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource TurnCompleted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool HeldRouteEvent { get; private set; }
        public int Size => _inner.Size;

        public void Arm() => Volatile.Write(ref _phase, 1);

        public void Release() => _release.Set();

        public void ReleaseAll()
        {
            _release.Set();
            _continue.Set();
        }

        public int Wait(Span<PollEvent> destination, TimeSpan timeout)
        {
            if (Interlocked.CompareExchange(ref _phase, 3, 2) == 2)
            {
                TurnCompleted.TrySetResult();
                _continue.Wait();
            }
            if (_deferred.Length != 0)
            {
                var deferredCount = Math.Min(destination.Length, _deferred.Length);
                _deferred.AsSpan(0, deferredCount).CopyTo(destination);
                _deferred = _deferred[deferredCount..];
                return deferredCount;
            }
            var count = _inner.Wait(destination, timeout);
            var timerCount = 0;
            for (var i = 0; i < count; i++)
                if (destination[i].SourceKind == PollSourceKind.Timer)
                    timerCount++;
            if (timerCount > 0 && Interlocked.CompareExchange(ref _phase, 2, 1) == 1)
            {
                _deferred = new PollEvent[count - timerCount];
                var timers = 0;
                var deferred = 0;
                for (var i = 0; i < count; i++)
                {
                    var ready = destination[i];
                    if (ready.SourceKind == PollSourceKind.Timer)
                    {
                        destination[timers++] = ready;
                        HeldRouteEvent |= (ready.Revents & PollEventFlags.PollRoute) != 0;
                    }
                    else
                        _deferred[deferred++] = ready;
                }
                count = timerCount;
                Held.TrySetResult();
                _release.Wait();
            }
            return count;
        }

        public void Add(IZlinkSocket socket, PollEventFlags events, nuint slot) =>
            _inner.Add(Unwrap(socket), events, slot);

        public void Add(ISocketMonitor monitor, PollEventFlags events, nuint slot) =>
            _inner.Add(monitor, events, slot);

        public void Add(IZlinkTimer timer, nuint slot) => _inner.Add(timer, slot);

        public void AddFd(int fd, PollEventFlags events, nuint slot) =>
            _inner.AddFd(fd, events, slot);

        public void Modify(IZlinkSocket socket, PollEventFlags events) =>
            _inner.Modify(Unwrap(socket), events);

        public void Modify(ISocketMonitor monitor, PollEventFlags events) =>
            _inner.Modify(monitor, events);

        public void ModifyFd(int fd, PollEventFlags events) => _inner.ModifyFd(fd, events);

        public bool Remove(IZlinkSocket socket) => _inner.Remove(Unwrap(socket));

        private static IZlinkSocket Unwrap(IZlinkSocket socket) =>
            socket is CapturingMeshSocketContext.CapturingRouterSocketProxy proxy
                ? proxy.Inner
                : socket;

        public bool Remove(ISocketMonitor monitor) => _inner.Remove(monitor);

        public bool Remove(IZlinkTimer timer) => _inner.Remove(timer);

        public bool Remove(int fd) => _inner.Remove(fd);

        public void Clear() => _inner.Clear();

        public void Close() => Dispose();

        public void Dispose()
        {
            _inner.Dispose();
            _release.Dispose();
            _continue.Dispose();
        }

        public ValueTask DisposeAsync()
        {
            Dispose();
            return ValueTask.CompletedTask;
        }
    }

    private static async Task ObserveAsync(Func<bool> condition)
    {
        var deadline = Stopwatch.GetTimestamp() + 5 * Stopwatch.Frequency;
        while (!condition())
        {
            Assert.True(
                Stopwatch.GetTimestamp() < deadline,
                "Expected public state was not observed."
            );
            await Task.Yield();
        }
    }
}
