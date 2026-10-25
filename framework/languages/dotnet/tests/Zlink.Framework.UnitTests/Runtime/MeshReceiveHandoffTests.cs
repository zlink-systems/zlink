using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshReceiveHandoffTests
{
    private static readonly TimeSpan CompletionBound = TimeSpan.FromSeconds(10);

    // Spec 08 §4.4: an idle resource takes the receive role before the previous
    // receiver enters the application's existing owner turn on that same thread.
    [Fact]
    public Task ReceiveRoleHandoff_RunsHandlerOnPreviousReceiverWhileNextReceiveProgresses() =>
        RunHandoffAsync(false);

    [Fact]
    public Task ReceiveRoleHandoff_WhenPoolIsFull_KeepsNativeReceiveAndDeadlineProgressing() =>
        RunHandoffAsync(true);

    private static async Task RunHandoffAsync(bool exhaustApplicationResources)
    {
        await using var nativeContext = Systems.Zlink.Zlink.CreateContext();
        var targetContext = new RecordingContext(nativeContext);
        var sourceAdmitted = Signal();
        var targetAdmitted = Signal();
        RecordingPoller? processPoller = null;
        ZLinkManagedMeshNode? sourceNode = null;
        ZLinkManagedMeshNode? targetNode = null;
        await using var source = sourceNode = new ZLinkManagedMeshNode(
            nativeContext,
            "receive-handoff",
            receivePollerFactory: () =>
                processPoller = new RecordingPoller(() =>
                {
                    if (sourceNode?.Status().AdmittedPeerCount == 1)
                        sourceAdmitted.TrySetResult();
                    if (targetNode?.Status().AdmittedPeerCount == 1)
                        targetAdmitted.TrySetResult();
                })
        );
        await using var target = targetNode = new ZLinkManagedMeshNode(
            targetContext,
            "receive-handoff",
            receivePollerFactory: () =>
                processPoller = new RecordingPoller(() =>
                {
                    if (targetNode?.Status().AdmittedPeerCount == 1)
                        targetAdmitted.TrySetResult();
                    if (sourceNode?.Status().AdmittedPeerCount == 1)
                        sourceAdmitted.TrySetResult();
                })
        );
        var sourceRid = RoutingId.From("handoff-source");
        var targetRid = RoutingId.From("handoff-target");
        var suffix = Guid.NewGuid().ToString("N");
        var targetEndpoint = $"inproc://handoff-target-{suffix}";
        source.SetRoutingId(sourceRid);
        target.SetRoutingId(targetRid);
        source.SetBind($"inproc://handoff-source-{suffix}");
        target.SetBind(targetEndpoint);
        source.ConnectPeer(targetEndpoint, targetRid);

        var handlerEntered = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var secondHandlerCompleted = Signal();
        var handlerCount = 0;
        using var releaseHandler = new ManualResetEventSlim();
        await using var pump = new ZLinkMeshDispatchPump(target, new ZLinkMeshCompletionTable());
        pump.SetNodeRouteHandler(
            (records, _) =>
            {
                foreach (var record in records)
                {
                    using (record)
                    {
                        Assert.Equal(sourceRid, record.SourceNodeRid);
                        Assert.Equal(1, record.PartCount);
                        Assert.True(record.ApplicationPayloadView.HasValue);
                        var marker = record.ApplicationPayloadView.Value.GetSpan(0)[0];
                        Assert.Equal(Interlocked.Increment(ref handlerCount), marker);
                        if (marker == 1)
                        {
                            handlerEntered.TrySetResult(Environment.CurrentManagedThreadId);
                            Assert.True(releaseHandler.Wait(CompletionBound));
                        }
                        else
                        {
                            Assert.Equal(2, marker);
                            secondHandlerCompleted.TrySetResult();
                        }
                    }
                }
                return ValueTask.CompletedTask;
            }
        );

        target.Start();
        source.Start();
        pump.EnsureStarted();
        var router = Assert.IsType<RecordingRouter>(targetContext.Router);
        var secondSubmitted = false;
        using var releaseApplicationResources = new ManualResetEventSlim();
        using var applicationResourcesEntered = new CountdownEvent(
            ZLinkProcessExecutionPool.WorkerCount - 1
        );
        var applicationSources = new List<ZLinkProcessExecutionPool.ApplicationSource>();
        var resourceFailures = new System.Collections.Concurrent.ConcurrentQueue<Exception>();
        try
        {
            await Task.WhenAll(sourceAdmitted.Task, targetAdmitted.Task).WaitAsync(CompletionBound);
            if (exhaustApplicationResources)
            {
                for (var index = 0; index < ZLinkProcessExecutionPool.WorkerCount - 1; index++)
                {
                    var application = ZLinkProcessExecutionPool.CreateApplicationSource(
                        _ =>
                        {
                            applicationResourcesEntered.Signal();
                            Assert.True(releaseApplicationResources.Wait(CompletionBound));
                            return ValueTask.CompletedTask;
                        },
                        resourceFailures.Enqueue
                    );
                    applicationSources.Add(application);
                    application.Signal();
                }
                Assert.True(applicationResourcesEntered.Wait(CompletionBound));
            }
            using var first = Message.From(new byte[] { 1 });
            Assert.Equal(SubmitResult.Ok, source.SendToNode(targetRid, [first]));
            var handlerThread = exhaustApplicationResources
                ? 0
                : await handlerEntered.Task.WaitAsync(CompletionBound);
            var receiveThread = await router.FirstReceived.Task.WaitAsync(CompletionBound);

            // The next physical receive must finish while the first handler is
            // still held. No elapsed timeout or posted-work count proves progress.
            using var second = Message.From(new byte[] { 2 });
            Assert.Equal(SubmitResult.Ok, source.SendToNode(targetRid, [second]));
            secondSubmitted = true;
            await router.SecondReceived.Task.WaitAsync(CompletionBound);
            Assert.False(releaseHandler.IsSet);
            Assert.False(secondHandlerCompleted.Task.IsCompleted);
            Assert.Equal(1, router.MaximumConcurrentReceives);
            Assert.Equal(2, router.NodeSendCount);
            Assert.Equal(1, processPoller!.MaximumConcurrentWaits);
            if (exhaustApplicationResources)
            {
                Assert.False(handlerEntered.Task.IsCompleted);
                Assert.False(releaseApplicationResources.IsSet);
                Assert.True(
                    source.TryBeginOperation(
                        MeshOperationKind.NodeRequest,
                        TimeSpan.FromMilliseconds(10),
                        out var operation
                    )
                );
                var expired = Signal();
                using var cancellation = operation.Token.Register(() => expired.TrySetResult());
                await expired.Task.WaitAsync(CompletionBound);
                Assert.False(handlerEntered.Task.IsCompleted);
            }
            else
                Assert.Equal(receiveThread, handlerThread);
        }
        finally
        {
            releaseApplicationResources.Set();
            releaseHandler.Set();
            foreach (var application in applicationSources)
                await application.DisposeAsync();
            Assert.Empty(resourceFailures);
            if (secondSubmitted)
            {
                await secondHandlerCompleted.Task.WaitAsync(CompletionBound);
                Assert.Equal(2, Volatile.Read(ref handlerCount));
            }
            await pump.DisposeAsync();
            await target.DisposeAsync();
            await source.DisposeAsync();
            Assert.Equal(0, processPoller!.MutationOverlapsWait);
            Assert.Equal(1, processPoller.MaximumConcurrentWaits);
        }
    }

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private sealed class RecordingRouter(IRouterSocket inner)
        : PressureSocket<IRouterSocket>(inner),
            IRouterSocket
    {
        private int _receiving;
        private int _maximumConcurrentReceives;
        private int _nodeSends;
        internal IRouterSocket Inner => InnerSocket;
        internal int MaximumConcurrentReceives => Volatile.Read(ref _maximumConcurrentReceives);
        internal int NodeSendCount => Volatile.Read(ref _nodeSends);
        internal TaskCompletionSource<int> FirstReceived { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource SecondReceived { get; } = Signal();
        public new RouterSocketOptions Options => InnerSocket.Options;

        public void SetRoutingId(RoutingId rid) => InnerSocket.SetRoutingId(rid);

        public RoutingId GetRoutingId() => InnerSocket.GetRoutingId();

        public SendOperation Send(RoutingId rid) => InnerSocket.Send(rid);

        public RequestOperation Request(RoutingId rid) => InnerSocket.Request(rid);

        public ReplyOperation Reply(RoutingId rid, ReplyToken token) =>
            InnerSocket.Reply(rid, token);

        public IReadOnlyList<RouterRoute> RoutesSnapshot() => InnerSocket.RoutesSnapshot();

        public new bool Recv(Received result, RecvFlags flags = RecvFlags.None)
        {
            var concurrent = Interlocked.Increment(ref _receiving);
            int observed;
            do
            {
                observed = Volatile.Read(ref _maximumConcurrentReceives);
                if (observed >= concurrent)
                    break;
            } while (
                Interlocked.CompareExchange(ref _maximumConcurrentReceives, concurrent, observed)
                != observed
            );
            try
            {
                if (!InnerSocket.Recv(result, flags))
                    return false;
                var header = result.Parts[0].AsReadOnlySpan();
                if (
                    ZLinkServiceWireCodec.TryDecodePrefix(header, out var command, out _, out _)
                    && command == ServiceWireConstants.Command.NodeSend
                )
                {
                    // The service wire packs payload into this frame. Match its
                    // NodeSend ordinal; the handler checks the decoded marker.
                    var ordinal = Interlocked.Increment(ref _nodeSends);
                    if (ordinal == 1)
                        FirstReceived.TrySetResult(Environment.CurrentManagedThreadId);
                    else if (ordinal == 2)
                        SecondReceived.TrySetResult();
                }
                return true;
            }
            finally
            {
                Interlocked.Decrement(ref _receiving);
            }
        }
    }

    private sealed class RecordingContext(IContext inner) : IContext
    {
        internal RecordingRouter? Router { get; private set; }
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

    private sealed class RecordingPoller(Action beforeWait) : IPoller
    {
        private readonly IPoller _inner = Systems.Zlink.Zlink.CreatePoller();
        private int _waiting;
        private int _maximumConcurrentWaits;
        internal int MaximumConcurrentWaits => Volatile.Read(ref _maximumConcurrentWaits);
        private int _mutationOverlapsWait;
        internal int MutationOverlapsWait => Volatile.Read(ref _mutationOverlapsWait);

        private void BeforeMutation()
        {
            if (Volatile.Read(ref _waiting) != 0)
                Interlocked.Increment(ref _mutationOverlapsWait);
        }

        private static IZlinkSocket Unwrap(IZlinkSocket socket) =>
            socket is RecordingRouter router ? router.Inner : socket;

        public int Size => _inner.Size;

        public void Add(IZlinkSocket socket, PollEventFlags events, nuint slot)
        {
            BeforeMutation();
            _inner.Add(Unwrap(socket), events, slot);
        }

        public void Add(ISocketMonitor monitor, PollEventFlags events, nuint slot)
        {
            BeforeMutation();
            _inner.Add(monitor, events, slot);
        }

        public void Add(IZlinkTimer timer, nuint slot)
        {
            BeforeMutation();
            _inner.Add(timer, slot);
        }

        public void AddFd(int fd, PollEventFlags events, nuint slot) =>
            _inner.AddFd(fd, events, slot);

        public void Modify(IZlinkSocket socket, PollEventFlags events)
        {
            BeforeMutation();
            _inner.Modify(Unwrap(socket), events);
        }

        public void Modify(ISocketMonitor monitor, PollEventFlags events)
        {
            BeforeMutation();
            _inner.Modify(monitor, events);
        }

        public void ModifyFd(int fd, PollEventFlags events) => _inner.ModifyFd(fd, events);

        public bool Remove(IZlinkSocket socket)
        {
            BeforeMutation();
            return _inner.Remove(Unwrap(socket));
        }

        public bool Remove(ISocketMonitor monitor)
        {
            BeforeMutation();
            return _inner.Remove(monitor);
        }

        public bool Remove(IZlinkTimer timer)
        {
            BeforeMutation();
            return _inner.Remove(timer);
        }

        public bool Remove(int fd) => _inner.Remove(fd);

        public void Clear()
        {
            BeforeMutation();
            _inner.Clear();
        }

        public void Close() => _inner.Close();

        public int Wait(Span<PollEvent> destination, TimeSpan timeout)
        {
            var waiting = Interlocked.Increment(ref _waiting);
            var previous = Volatile.Read(ref _maximumConcurrentWaits);
            while (waiting > previous)
            {
                var observed = Interlocked.CompareExchange(
                    ref _maximumConcurrentWaits,
                    waiting,
                    previous
                );
                if (observed == previous)
                    break;
                previous = observed;
            }
            try
            {
                beforeWait();
                return _inner.Wait(destination, timeout);
            }
            finally
            {
                Interlocked.Decrement(ref _waiting);
            }
        }

        public void Dispose()
        {
            BeforeMutation();
            _inner.Dispose();
        }

        public ValueTask DisposeAsync() => _inner.DisposeAsync();
    }
}
