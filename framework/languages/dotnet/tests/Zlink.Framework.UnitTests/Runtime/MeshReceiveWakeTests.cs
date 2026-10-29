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
    private static readonly TimeSpan RegistrationTimeout = TimeSpan.FromSeconds(30);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task RegistrationCancellationKeepsOriginalTokenAndOneTerminal(bool alreadyCanceled)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "registration-cancel");
        var actor = CreateRegistrationActor(node, "registration-cancel-actor");
        using var request = Message.From("registration request");
        for (var iteration = 0; iteration < 64; iteration++)
        {
            using var cancellation = new CancellationTokenSource();
            if (alreadyCanceled)
                cancellation.Cancel();
            var cancel = alreadyCanceled ? Task.CompletedTask : Task.Run(cancellation.Cancel);
            var completion = RequestActor(node, actor, request, cancellation.Token, static _ => 7);
            await cancel;
            var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => completion);
            Assert.Equal(cancellation.Token, error.CancellationToken);
            Assert.True(completion.IsCanceled);
            await node.Completions.CompletionDrained.WaitAsync(CompletionBound);
            Assert.Equal(0, node.PendingOperationCount);
            DrainApplicationRequests(node);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task InvalidRegistrationCompletesOriginalCallerWithoutDecoding(
        bool invalidLifecycle
    )
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "registration-invalid");
        var valid = node.AllocateOperationId();
        var invalid = invalidLifecycle
            ? new MeshOperationId(valid.High ^ 1UL, valid.Low)
            : new MeshOperationId(valid.High, 0);
        var completion = node.RequestDirectWireAsync(
                RoutingId.From("registration-invalid-target"),
                Array.Empty<Message>(),
                RegistrationTimeout,
                CancellationToken.None,
                operationId: invalid
            )
            .AsTask();
        await Assert.ThrowsAsync<ArgumentException>(() => completion);
        await node.Completions.CompletionDrained.WaitAsync(CompletionBound);
        Assert.Equal(0, node.PendingOperationCount);
    }

    [Fact]
    public async Task DuplicateRegistrationCannotRemoveOriginalOperation()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        using var server = context.CreateRouterSocket();
        using var client = context.CreateRouterSocket();
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        await using var node = new ZLinkManagedMeshNode(
            context,
            "registration-duplicate",
            directRequestSocket: client
        );
        var target = RoutingId.From("registration-duplicate-target");
        server.SetRoutingId(target);
        client.SetRoutingId(RoutingId.From("registration-duplicate-source"));
        server.Options.ReceiveTimeout = CompletionBound;
        var endpoint = $"inproc://registration-duplicate-{Guid.NewGuid():N}";
        server.Bind(endpoint);
        poller.Add(client, PollEventFlags.PollRoute | PollEventFlags.PollCompletion, 1);
        client.Connect(endpoint);
        var events = new PollEvent[1];
        while (client.RoutesSnapshot().Count == 0)
            Assert.True(poller.Wait(events, CompletionBound) > 0);
        poller.Modify(client, PollEventFlags.PollCompletion);
        var id = node.AllocateOperationId();
        using var request = Message.From("first");
        var first = node.RequestDirectWireAsync(
                target,
                [request],
                RegistrationTimeout,
                CancellationToken.None,
                operationId: id
            )
            .AsTask();
        using var received = Received.Create();
        Assert.True(server.Recv(received));
        Assert.Equal(
            "first",
            System.Text.Encoding.UTF8.GetString(received.SinglePartOrThrow().AsReadOnlySpan())
        );
        var duplicate = node.RequestDirectWireAsync(
                target,
                Array.Empty<Message>(),
                RegistrationTimeout,
                CancellationToken.None,
                operationId: id
            )
            .AsTask();
        await Assert.ThrowsAsync<InvalidOperationException>(() => duplicate);
        Assert.False(first.IsCompleted);
        Assert.Equal(1, node.PendingOperationCount);
        using var reply = Message.From(BitConverter.GetBytes(true));
        received.Reply().Message(reply).Submit();
        Assert.True(poller.Wait(events, CompletionBound) > 0);
        var parts = await first.WaitAsync(CompletionBound);
        try
        {
            Assert.True(BitConverter.ToBoolean(Assert.Single(parts).AsReadOnlySpan()));
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
        await node.Completions.CompletionDrained.WaitAsync(CompletionBound);
        Assert.Equal(0, node.PendingOperationCount);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task WakeFailureUsesOnlyTheAtomicTakeWinner(bool cancelBeforeFailure)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "registration-wake-failure");
        var actor = CreateRegistrationActor(node, "registration-wake-actor");
        using var request = Message.From("registration request");
        using var cancellation = new CancellationTokenSource();
        var decodes = 0;
        var failure = new InvalidOperationException("wake observer failure");
        node.ReceiveWakeObserver = () =>
        {
            if (cancelBeforeFailure)
                cancellation.Cancel();
            throw failure;
        };
        var completion = RequestActor(node, actor, request, cancellation.Token, _ => ++decodes);
        node.ReceiveWakeObserver = null;
        if (cancelBeforeFailure)
        {
            var canceled = await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                completion
            );
            Assert.Equal(cancellation.Token, canceled.CancellationToken);
        }
        else
            Assert.Same(
                failure,
                await Assert.ThrowsAsync<InvalidOperationException>(() => completion)
            );
        await node.Completions.CompletionDrained.WaitAsync(CompletionBound);
        Assert.Equal(0, node.PendingOperationCount);
        Assert.Equal(0, decodes);
        Assert.Empty(DrainApplicationRequests(node));
    }

    [Fact]
    public async Task ContendedRegistrationObservesCancellationBeforeItPublishes()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var lane = new ZLinkStateLane();
        await using var node = new ZLinkManagedMeshNode(
            context,
            "registration-contended",
            operationLane: lane
        );
        var actor = CreateRegistrationActor(node, "registration-contended-actor");
        using var request = Message.From("registration request");
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var holding = lane.RunAsync(async () =>
        {
            entered.SetResult();
            await release.Task;
        });
        await entered.Task.WaitAsync(CompletionBound);
        using var cancellation = new CancellationTokenSource();
        var decodes = 0;
        var completion = RequestActor(node, actor, request, cancellation.Token, _ => ++decodes);
        try
        {
            Assert.False(completion.IsCompleted);
            cancellation.Cancel();
        }
        finally
        {
            release.SetResult();
        }
        await holding;
        var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => completion);
        Assert.Equal(cancellation.Token, error.CancellationToken);
        await node.Completions.CompletionDrained.WaitAsync(CompletionBound);
        Assert.Equal(0, node.PendingOperationCount);
        Assert.Equal(0, decodes);
        Assert.Empty(DrainApplicationRequests(node));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ActorReplyCodecSurvivesCancellationBeforePhysicalSubmission(bool throwing)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var clock = new BeforeActorSubmitTimeProvider();
        await using var node = new ZLinkManagedMeshNode(
            context,
            "actor-codec-before-submit",
            deadlineTimeProvider: clock
        );
        var actor = CreateRegistrationActor(node, "actor-codec-before-submit-actor");
        using var request = Message.From("request");
        using var cancellation = new CancellationTokenSource();
        var decodes = 0;
        IReadOnlyList<Message>? decodedParts = null;
        var barrierEntered = false;
        node.ReceiveWakeObserver = () =>
        {
            node.ReceiveWakeObserver = null;
            // Registration has returned from the operation lane. The next timestamp
            // is the deadline check after the original exact-pair lookup succeeds.
            clock.BeforeTimestamp = () =>
            {
                barrierEntered = true;
                cancellation.Cancel();
                node.Completions.CompletionDrained.WaitAsync(CompletionBound)
                    .GetAwaiter()
                    .GetResult();
                Assert.Equal(0, node.PendingOperationCount);
            };
        };
        var completion = RequestActor(
            node,
            actor,
            request,
            cancellation.Token,
            parts =>
            {
                decodes++;
                decodedParts = parts;
                var value = BitConverter.ToInt32(Assert.Single(parts).AsReadOnlySpan());
                if (throwing)
                    throw new InvalidOperationException("late codec failure");
                return value;
            }
        );
        Assert.True(barrierEntered);
        var original = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => completion);
        Assert.Equal(cancellation.Token, original.CancellationToken);
        Assert.Equal(0, decodes);
        var submissions = 0;
        using var reply = Message.From(BitConverter.GetBytes(7));
        using (var ready = new MeshReadyBatch())
        {
            node.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
            for (var index = 0; index < ready.Count; index++)
            {
                using var claim = ready.TakeClaim(index);
                using var received = new MeshReceiveBatch();
                while (claim.Receive(received, RecvFlags.DontWait))
                {
                    for (var record = 0; record < received.Count; record++)
                    {
                        Assert.Equal(MeshRecordKind.ActorRequest, received[record].Kind);
                        submissions++;
                        Assert.Equal(SubmitResult.Ok, received[record].Reply([reply]));
                    }
                    received.Reset();
                }
            }
        }
        Assert.Equal(1, submissions);
        Assert.Equal(1, decodes);
        Assert.NotNull(decodedParts);
        Assert.All(
            decodedParts,
            part => Assert.Throws<ObjectDisposedException>(() => part.AsReadOnlySpan())
        );
        Assert.Equal(0, node.PendingOperationCount);
        Assert.True(completion.IsCanceled);
        var stillCanceled = await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            completion
        );
        Assert.Equal(cancellation.Token, stillCanceled.CancellationToken);
        await node.Completions.CompletionDrained.WaitAsync(CompletionBound);
        Assert.Empty(DrainApplicationRequests(node));
    }

    private sealed class BeforeActorSubmitTimeProvider : TimeProvider
    {
        internal Action? BeforeTimestamp;

        public override long GetTimestamp()
        {
            var before = Interlocked.Exchange(ref BeforeTimestamp, null);
            before?.Invoke();
            return 0;
        }
    }

    [Fact]
    public async Task DuplicateRelocationRegistrationPreservesTheOriginalReference()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            "registration-relocation-duplicate"
        );
        var source = SetRequestSource(node);
        var actor = CreateRegistrationActor(node, "registration-relocation-actor");
        using var request = Message.From("registration request");
        using var cancellation = new CancellationTokenSource();
        var decodes = 0;
        var first = RequestActor(node, actor, request, cancellation.Token, _ => ++decodes);
        var id = Assert.Single(DrainApplicationRequests(node));
        Assert.False(first.IsCompleted);
        Assert.Equal(1, node.PendingOperationCount);
        cancellation.Cancel();
        var canceled = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => first);
        Assert.Equal(cancellation.Token, canceled.CancellationToken);
        Assert.Equal(0, node.PendingOperationCount);
        Assert.Throws<InvalidOperationException>(() =>
            node.RequestToActor(actor, [request], id, RegistrationTimeout)
        );
        var payload = new[] { Message.From("late") };
        var result = node.TryCompleteRelocationReply(Relay(id), payload);
        Assert.Equal(ZLinkRelocationReplyCompletionState.AlreadyTerminal, result.State);
        Assert.Equal(source, result.RequestSource);
        Assert.Throws<ObjectDisposedException>(() => payload[0].AsReadOnlySpan());
        Assert.True(first.IsCanceled);
        var stillCanceled = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => first);
        Assert.Equal(cancellation.Token, stillCanceled.CancellationToken);
        Assert.Equal(0, decodes);
        Assert.Empty(DrainApplicationRequests(node));
        await node.Completions.CompletionDrained.WaitAsync(CompletionBound);
        Assert.Equal(0, node.PendingOperationCount);
    }

    [Fact]
    public async Task CancellationRejectsLateDecoderAndReleasesDispatcherReservation()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "registration-late-decoder");
        var source = SetRequestSource(node);
        var actor = CreateRegistrationActor(node, "registration-late-decoder-actor");
        using var request = Message.From("registration request");
        using var cancellation = new CancellationTokenSource();
        var decodes = 0;
        int Decode(IReadOnlyList<Message> parts)
        {
            Interlocked.Increment(ref decodes);
            return BitConverter.ToInt32(Assert.Single(parts).AsReadOnlySpan());
        }
        var completion = RequestActor(node, actor, request, cancellation.Token, Decode);
        var id = Assert.Single(DrainApplicationRequests(node));
        cancellation.Cancel();
        var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => completion);
        Assert.Equal(cancellation.Token, error.CancellationToken);
        await node.Completions.CompletionDrained.WaitAsync(CompletionBound);
        var latePayload = new[] { Message.From(BitConverter.GetBytes(7)) };
        var late = node.TryCompleteRelocationReply(Relay(id), latePayload);
        Assert.Equal(ZLinkRelocationReplyCompletionState.AlreadyTerminal, late.State);
        Assert.Equal(source, late.RequestSource);
        Assert.Throws<ObjectDisposedException>(() => latePayload[0].AsReadOnlySpan());
        Assert.Equal(0, decodes);
        Assert.True(completion.IsCanceled);
        var stillCanceled = await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            completion
        );
        Assert.Equal(cancellation.Token, stillCanceled.CancellationToken);
        Assert.Equal(0, node.PendingOperationCount);

        var live = RequestActor(node, actor, request, CancellationToken.None, Decode);
        var liveId = Assert.Single(DrainApplicationRequests(node));
        var livePayload = new[] { Message.From(BitConverter.GetBytes(7)) };
        var accepted = node.TryCompleteRelocationReply(Relay(liveId), livePayload);
        Assert.Equal(ZLinkRelocationReplyCompletionState.TerminalReceived, accepted.State);
        Assert.Equal(source, accepted.RequestSource);
        Assert.Equal(7, await live.WaitAsync(CompletionBound));
        Assert.Equal(1, decodes);
        Assert.Throws<ObjectDisposedException>(() => livePayload[0].AsReadOnlySpan());
        await node.Completions.CompletionDrained.WaitAsync(CompletionBound);
        Assert.Equal(0, node.PendingOperationCount);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    public async Task RegistrationClockFailureCompletesOriginalCallerAndRemovesPublishedEntries(
        int pendingCountAtFailure
    )
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var clock = new RegistrationFailureTimeProvider();
        await using var node = new ZLinkManagedMeshNode(
            context,
            "registration-clock-failure",
            deadlineTimeProvider: clock
        );
        var actor = CreateRegistrationActor(node, "registration-clock-failure-actor");
        using var request = Message.From("registration request");
        var failure = new InvalidOperationException("registration clock failure");
        clock.FailWhen = () => node.PendingOperationCount == pendingCountAtFailure;
        clock.Failure = failure;
        var decodes = 0;
        var completion = RequestActor(node, actor, request, CancellationToken.None, _ => ++decodes);
        Assert.Same(failure, await Assert.ThrowsAsync<InvalidOperationException>(() => completion));
        Assert.True(clock.Failed);
        await node.Completions.CompletionDrained.WaitAsync(CompletionBound);
        Assert.Equal(0, node.PendingOperationCount);
        Assert.Equal(0, decodes);
        Assert.Empty(DrainApplicationRequests(node));
    }

    [Fact]
    public async Task BaseRegistrationPreservesWakeFailureThrowSurface()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "base-registration-wake-failure");
        var failure = new InvalidOperationException("base wake failure");
        node.ReceiveWakeObserver = () => throw failure;
        Assert.Same(
            failure,
            Assert.Throws<InvalidOperationException>(() =>
                node.TryBeginOperation(
                    MeshOperationKind.SpotRequest,
                    TimeSpan.FromSeconds(30),
                    out _
                )
            )
        );
        node.ReceiveWakeObserver = null;
        Assert.Equal(0, node.PendingOperationCount);
        await node.Completions.CompletionDrained.WaitAsync(CompletionBound);
    }

    private static ActorRef CreateRegistrationActor(ZLinkManagedMeshNode node, string actorId)
    {
        if (node.RoutingId.IsEmpty)
            node.SetRoutingId(RoutingId.From(actorId));
        var actor = node.CreateActor(actorId);
        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
        for (var index = 0; index < ready.Count; index++)
        {
            using var claim = ready.TakeClaim(index);
            using var received = new MeshReceiveBatch();
            Assert.True(claim.Receive(received, RecvFlags.DontWait));
            for (var record = 0; record < received.Count; record++)
                Assert.Equal(MeshRecordKind.SpotControl, received[record].Kind);
        }
        return actor;
    }

    private static Task<int> RequestActor(
        ZLinkManagedMeshNode node,
        ActorRef actor,
        Message request,
        CancellationToken token,
        Func<IReadOnlyList<Message>, int> decode
    ) =>
        node.RequestToActorAsync(
            actor,
            [request],
            RegistrationTimeout,
            token,
            decode,
            static error => error,
            null
        );

    private static MeshOperationId[] DrainApplicationRequests(ZLinkManagedMeshNode node)
    {
        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
        var ids = new List<MeshOperationId>();
        for (var index = 0; index < ready.Count; index++)
        {
            using var claim = ready.TakeClaim(index);
            using var received = new MeshReceiveBatch();
            Assert.True(claim.Receive(received, RecvFlags.DontWait));
            for (var record = 0; record < received.Count; record++)
            {
                Assert.Equal(MeshRecordKind.ActorRequest, received[record].Kind);
                ids.Add(received[record].OperationId);
            }
        }
        return ids.ToArray();
    }

    private static ZLinkServiceWireCodec.RequestSourceFence SetRequestSource(
        ZLinkManagedMeshNode node
    )
    {
        node.SetRoutingId(RoutingId.From("registration-relay-source"));
        var source = new ZLinkServiceWireCodec.RequestSourceFence(
            "registration-relay-owner",
            1,
            node.RoutingId,
            node.AllocateOperationId().High
        );
        node.SetLocalRequestSourceFence(source);
        return source;
    }

    private static ZLinkServiceWireCodec.ReplyRelayRecord Relay(MeshOperationId id) =>
        new(
            id,
            id.Low,
            new ZLinkServiceWireCodec.RelocationWireId(1, 1),
            1,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                "registration-relay-target",
                1,
                RoutingId.From("registration-relay-target"),
                1,
                "registration-relay-root"
            ),
            1,
            1,
            (uint)RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None
        );

    private sealed class RegistrationFailureTimeProvider : TimeProvider
    {
        internal Func<bool>? FailWhen;
        internal Exception? Failure;
        internal bool Failed;

        public override long GetTimestamp()
        {
            if (!Failed && Failure is { } error && FailWhen!())
            {
                Failed = true;
                throw error;
            }
            return 0;
        }
    }

    [Fact]
    public async Task IdleReceiveOwnerWaitsForReadinessWithoutAPeriodicDeadline()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                typeof(MeshReceiveWakeTests),
                nameof(IdleReceiveOwnerWaitsForReadinessWithoutAPeriodicDeadline)
            )
        )
            return;
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var poller = new ControlledReceivePoller { CaptureIdleWait = true };
        // Pool bootstrap can wait while monitor registration is still queued.
        // Measure the configured node's idle wait, preserving the same deadline assertion.
        poller.CanObserveIdleWait = () => poller.MonitorRegistered.Task.IsCompleted;
        await using var node = new ZLinkManagedMeshNode(
            context,
            "idle-wait",
            receivePollerFactory: () => poller
        );
        node.SetRoutingId(RoutingId.From("idle-wait"));
        node.Start();
        await poller.MonitorRegistered.Task.WaitAsync(CompletionBound);
        Assert.Equal(
            Timeout.InfiniteTimeSpan,
            await poller.IdleWait.Task.WaitAsync(CompletionBound)
        );
    }

    [Fact]
    public async Task IdleWaitObservationPreservesFiniteDeadlinesAndSkipsNonblockingTurns()
    {
        using var poller = new ControlledReceivePoller { CaptureIdleWait = true };
        var events = new PollEvent[1];
        Assert.Equal(0, poller.Wait(events, TimeSpan.Zero));
        Assert.False(poller.IdleWait.Task.IsCompleted);
        var nonblocking = Task.Run(() => poller.Wait(events, TimeSpan.Zero));
        try
        {
            await Task.WhenAny(nonblocking, poller.IdleWait.Task).WaitAsync(CompletionBound);
            Assert.False(poller.IdleWait.Task.IsCompleted);
            Assert.Equal(0, await nonblocking.WaitAsync(CompletionBound));
        }
        finally
        {
            if (!nonblocking.IsCompleted)
            {
                poller.ReleaseWait();
                await nonblocking.WaitAsync(CompletionBound);
            }
        }
        var finite = TimeSpan.FromMilliseconds(1);
        Assert.Equal(0, poller.Wait(events, finite));
        Assert.Equal(finite, await poller.IdleWait.Task.WaitAsync(CompletionBound));
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
        if (
            failRearm
            && await IsolatedTestProcess.RunIsolatedAsync(
                typeof(MeshReceiveWakeTests),
                nameof(PermitGrantResumesReceiveOwnerWithoutAPollTick)
            )
        )
            return;
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
            var receiptsPerDrain = new int[iterations];
            var drains = 0;
            // Alternating windows give both sides the same JIT tiering state.
            var (actual, baseline) = PerCallAllocation.MinWindowPair(
                index =>
                {
                    drains++;
                    var before = router.Calls;
                    router.Remaining = 1;
                    do node.DrainRawSocket(CancellationToken.None);
                    while (router.Remaining != 0);
                    receiptsPerDrain[index] = router.Calls - before;
                },
                // Match the number of receipt containers each bounded drain really
                // attempted; no platform-specific object size is embedded here.
                index =>
                {
                    for (
                        var receiptIndex = 0;
                        receiptIndex < receiptsPerDrain[index];
                        receiptIndex++
                    )
                    {
                        using var receipt = Received.Create();
                        if (receiptIndex == 0)
                            _ = receipt.Parts;
                        _ = receipt.RoutingId;
                    }
                },
                iterations
            );
            Assert.Equal(drains, router.Accepted - 128);
            Assert.True(router.Calls - calls >= drains);
            Assert.True(
                actual == baseline,
                $"ingress={actual}B, receipt baseline={baseline}B, excess={actual - baseline}B/window"
            );
        }
        finally
        {
            var shutdown = node.DisposeAsync();
            poller.ReleaseWait();
            await shutdown;
        }
    }

    // Supervisor ruling for #2758: adapting this contract to spec 01 §10.1 is
    // classification A. After the reserved empty visit (if any), spec 01 §10.1
    // requires zero idle operation-owner turns. This node starts without a reservation. The
    // allocation baseline therefore includes only liveness's existing turn.
    // The second phase adds one turn and must fail that same cost assertion.
    [Fact]
    public async Task IdleReceiveRotationManagementAddsNoAllocationBeyondItsNodeTurns()
    {
        const int warmup = 128;
        const int iterations = 1000;
        var poller = new ControlledReceivePoller();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var operationLane = new ZLinkStateLane();
        var node = new ZLinkManagedMeshNode(
            context,
            "idle-rotation-allocation",
            receivePollerFactory: () => poller,
            operationLane: operationLane
        );
        node.SetRoutingId(RoutingId.From("idle-rotation-allocation"));
        await using var lane = new ZLinkStateLane();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var occupied = operationLane.RunAsync(async () =>
        {
            entered.SetResult();
            await release.Task;
        });
        await entered.Task;
        var addTurn = false;
        long actual = 0;
        long baseline = 0;
        var rotations = 0;
        long last = 0;
        SemaphoreSlim rotated = new(0);
        // The window opens after the controlled poller returns and closes before
        // its next wait, so it holds only the node's own rotation.
        poller.AfterWait = () =>
        {
            last = GC.GetAllocatedBytesForCurrentThread();
            if (addTurn)
                _ = lane.RunAsync(static value => value, 0).GetAwaiter().GetResult();
        };
        poller.BeforeWait = () =>
        {
            var now = GC.GetAllocatedBytesForCurrentThread();
            var measured = now - last;
            var start = GC.GetAllocatedBytesForCurrentThread();
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
            addTurn = true;
            actual = baseline = 0;
            rotations = 0;
            for (var index = 0; index < warmup + iterations; index++)
            {
                node.SignalReceiveOwner();
                Assert.True(await rotated.WaitAsync(CompletionBound));
            }
            Assert.Throws<Xunit.Sdk.TrueException>(() => Assert.True(actual == baseline));
            output.WriteLine(
                $"negative control: extra owner turn actual={actual}B baseline={baseline}B"
            );
        }
        finally
        {
            release.SetResult();
            await occupied;
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
            var actualSamples = new List<long>();
            var baselineSamples = new List<long>();
            long lastActual = 0;
            long lastBaseline = 0;
            var lastPaused = false;
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
                lastPaused = GC.GetTotalPauseDuration() != router.RecordPause;
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
                // A sample that overlapped a GC pause can carry a retired allocation context.
                if (index >= warmup && !lastPaused)
                {
                    actualSamples.Add(lastActual);
                    baselineSamples.Add(lastBaseline);
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
            var actual = PerCallAllocation.MinWindow(actualSamples);
            var baseline = PerCallAllocation.MinWindow(baselineSamples);
            output.WriteLine(
                $"node ingress={actual}B, lane + owner baseline={baseline}B, excess={actual - baseline}B/window"
            );
            Assert.True(
                actual == baseline,
                $"node ingress={actual}B, lane + owner baseline={baseline}B, excess={actual - baseline}B/window"
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
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                typeof(MeshReceiveWakeTests),
                nameof(PoolWaitFailureTerminatesNodeAndJoinsTheReceiveWorker)
            )
        )
            return;
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
        internal TimeSpan RecordPause;
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
                        RecordPause = GC.GetTotalPauseDuration();
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
        internal Func<bool>? CanObserveIdleWait { get; set; }
        internal readonly TaskCompletionSource MonitorRegistered = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
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
                // Zero-time turns apply startup registrations and pending wakes.
                // Observe the first blocking wait, preserving even a finite deadline.
                if (timeout != TimeSpan.Zero && (CanObserveIdleWait?.Invoke() ?? true))
                    IdleWait.TrySetResult(timeout);
                BeforeWait?.Invoke();
                return _inner.Wait(destination, timeout);
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
            _inner.Add(monitor, events, slot);
            MonitorRegistered.TrySetResult();
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
