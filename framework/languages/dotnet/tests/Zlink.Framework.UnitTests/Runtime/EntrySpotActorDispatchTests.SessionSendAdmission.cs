using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Messaging;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public async Task SessionSendAdmission_SecondSendCompletesWhileFirstHandlerHoldsTurn()
    {
        var probe = new DispatchProbe();
        var (runtime, bound, state) = await CreateSessionSendFixtureAsync(probe);
        Task? first = null;
        Task? second = null;
        try
        {
            first = RelaySessionSend(bound, "first");
            await probe.ActorAFirstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            second = RelaySessionSend(bound, "second");
            await second.WaitAsync(TimeSpan.FromSeconds(1));
            Assert.True(first.IsCompletedSuccessfully);
            Assert.False(probe.ActorASecondStarted.Task.IsCompleted);

            probe.ReleaseActorAFirst.SetResult();
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            Assert.Equal(
                ["actor-a:first:start", "actor-a:first:end", "actor-a:second:start"],
                probe.Events
            );
        }
        finally
        {
            probe.ReleaseActorAFirst.TrySetResult();
            try
            {
                await Task.WhenAll(first ?? Task.CompletedTask, second ?? Task.CompletedTask);
            }
            finally
            {
                await runtime.StopAsync(CancellationToken.None);
            }
        }
    }

    [Theory]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor)]
    public async Task SessionSendPlacement_AcceptedSendsRunBeforeDisconnect(
        ZLinkUserSpotExecutionMode mode
    )
    {
        var probe = new DispatchProbe();
        var (runtime, bound, _) = await CreateSessionSendFixtureAsync(probe, userSpotMode: mode);
        Task? disconnect = null;
        try
        {
            await RelaySessionSend(bound, "first").WaitAsync(TimeSpan.FromSeconds(1));
            await probe.ActorAFirstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await RelaySessionSend(bound, "second").WaitAsync(TimeSpan.FromSeconds(1));

            disconnect = bound.NotifyDisconnectedAsync().AsTask();
            Assert.False(probe.ActorASecondStarted.Task.IsCompleted);
            probe.ReleaseActorAFirst.SetResult();

            await disconnect.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(
                ["actor-a:first:start", "actor-a:first:end", "actor-a:second:start", "disconnect"],
                probe.Events
            );
        }
        finally
        {
            probe.ReleaseActorAFirst.TrySetResult();
            if (disconnect is not null)
                await Task.WhenAny(disconnect);
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(null)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor)]
    public async Task SessionSendAdmission_ClosedMailboxRejectsSendWithLiveBinding(
        ZLinkUserSpotExecutionMode? userSpotMode
    )
    {
        var probe = new DispatchProbe();
        var (runtime, bound, state) = await CreateSessionSendFixtureAsync(
            probe,
            userSpotMode: userSpotMode
        );
        try
        {
            var barrier = state._dispatchMailbox.CloseAdmissionAndReserveLifecycleBarrier();
            ZLinkFrameworkException error;
            using (await barrier.ClaimAsync())
            {
                Assert.True(
                    runtime.TryGetSessionActorBinding(bound.ActorId, bound.BindingToken, out _)
                );
                error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                    RelaySessionSend(bound, "second")
                );
            }
            await barrier.Completion;
            state._dispatchMailbox.ReopenAdmission();
            Assert.Equal(ZLinkFrameworkErrorKind.Rejected, error.Kind);
            Assert.Empty(probe.Events);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(null)]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide)]
    public async Task SessionSendAdmission_HandlerExceptionRecordsDispatchErrorAfterAcceptance(
        ZLinkUserSpotExecutionMode? userSpotMode
    )
    {
        var probe = new DispatchProbe
        {
            SecondSendFailure = new InvalidOperationException("session-send-handler-failed"),
        };
        using var observer = new CapturingMessageFlowObserver();
        var (runtime, bound, state) = await CreateSessionSendFixtureAsync(
            probe,
            observer,
            userSpotMode
        );
        var failures = new AuditRuntimeFailureReporter();
        runtime.ErrorSink.UnhandledCallbackException += failures.ReportHandlerException;
        try
        {
            await RelaySessionSend(bound, "second");
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);

            var failure = Assert.Single(
                observer.Events,
                flow =>
                    flow.ActorId == bound.ActorId
                    && flow.PacketName == "second"
                    && flow.Outcome == "failed"
            );
            Assert.Equal("actor", failure.Surface);
            Assert.Equal("send", failure.MessageKind);
            Assert.Equal("drop", failure.Action);
            Assert.Equal("handler_exception", failure.Reason);
            Assert.Equal(probe.SecondSendFailure.Message, failure.ErrorMessage);
            Assert.Equal("actor-a:second:start", Assert.Single(probe.Events));
            Assert.Empty(failures.Failures);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SessionSendAdmission_JoinSealAfterAcceptanceRecordsDispatchError()
    {
        var probe = new DispatchProbe();
        var reported = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var observer = new CapturingMessageFlowObserver(flow =>
        {
            if (IsSecondSendFailure(flow))
                reported.TrySetResult();
        });
        var (runtime, bound, state) = await CreateSessionSendFixtureAsync(
            probe,
            observer,
            ZLinkUserSpotExecutionMode.PerActor
        );
        var failures = new AuditRuntimeFailureReporter();
        runtime.ErrorSink.UnhandledCallbackException += failures.ReportHandlerException;
        try
        {
            await RelaySessionSend(bound, "first").WaitAsync(TimeSpan.FromSeconds(5));
            await probe.ActorAFirstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await RelaySessionSend(bound, "second").WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(probe.ActorASecondStarted.Task.IsCompleted);

            state.SealFailedJoinCompletion(new InvalidOperationException("join-completion-failed"));
            probe.ReleaseActorAFirst.SetResult();
            await Task.WhenAny(reported.Task, Task.Delay(TimeSpan.FromSeconds(5)));

            var failure = Assert.Single(observer.Events.Where(IsSecondSendFailure));
            Assert.Equal("actor", failure.Surface);
            Assert.Equal("send", failure.MessageKind);
            Assert.Equal("drop", failure.Action);
            Assert.Equal("handler_exception", failure.Reason);
            Assert.Contains("sealed after Join completion failed", failure.ErrorMessage);
            Assert.False(probe.ActorASecondStarted.Task.IsCompleted);
            Assert.Empty(failures.Failures);
        }
        finally
        {
            probe.ReleaseActorAFirst.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }

        static bool IsSecondSendFailure(ObservedMessageFlow flow) =>
            flow.ActorId == "actor-a" && flow.PacketName == "second" && flow.Outcome == "failed";
    }

    [Fact]
    public async Task SessionSendAdmission_RemovedBindingRejectsSend()
    {
        var probe = new DispatchProbe();
        var (runtime, bound, state) = await CreateSessionSendFixtureAsync(probe);
        try
        {
            await bound.NotifyDisconnectedAsync();
            Assert.False(
                runtime.TryGetSessionActorBinding(bound.ActorId, bound.BindingToken, out _)
            );
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                RelaySessionSend(bound, "second")
            );
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            Assert.Empty(probe.Events);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private static async Task<(
        ZLinkFrameworkRuntime Runtime,
        ZLinkSessionActor Bound,
        ZLinkActorRuntimeState State
    )> CreateSessionSendFixtureAsync(
        DispatchProbe probe,
        CapturingMessageFlowObserver? observer = null,
        ZLinkUserSpotExecutionMode? userSpotMode = null
    )
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            messageFlowObserver: observer,
            entrySpotType: typeof(ProbeEntrySpot),
            dispatchProbe: probe,
            userSpotType: userSpotMode is null ? null : typeof(SessionSendSpot),
            userSpotExecutionMode: userSpotMode ?? ZLinkUserSpotExecutionMode.SpotWide
        );
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            if (userSpotMode is not null)
            {
                var created = await runtime.CreateAsync<SessionSendSpot>();
                var joined = await GetSpotActivation(runtime, created.Spot.SpotId)
                    .JoinActorAsync(actor, ZLinkMessage.Empty, CancellationToken.None);
                Assert.True(joined.Accepted);
            }
            var sessionRid = RoutingId.From("session-send-admission");
            var context = new ZLinkSessionContext(
                runtime,
                new ZLinkManagedStream(
                    new RelayStreamSocket(),
                    sessionRid,
                    runtime.Registration.Codecs,
                    "test"
                ),
                new RelaySessionHandlerRegistry(),
                static () => ValueTask.CompletedTask,
                static _ => ValueTask.CompletedTask
            );
            var bound = new ZLinkSessionActor(
                context,
                actorRef.ActorId,
                sessionRid,
                "send-binding"
            );
            _ = runtime.BindSessionActor(
                actorRef.ActorId,
                context,
                bound.BindingToken,
                bound,
                bindingGeneration: 1,
                route: ZLinkSessionBindingRoute.Create(
                    new ActorRef(actorRef.ActorId, actorRef.Generation, "entry", actorRef.NodeRid),
                    "entry",
                    targetNodeGeneration: 1,
                    authorityOwnerGeneration: 1,
                    ownerLeaseGeneration: 1
                ),
                sessionOwnerNodeGeneration: 1,
                sessionOwnerNodeRid: node.RoutingId,
                sessionOwnerId: "session-owner",
                sessionOwnerLeaseGeneration: 1
            );
            return (runtime, bound, runtime.GetOrCreateActorState(actorRef.ActorId));
        }
        catch
        {
            await runtime.StopAsync(CancellationToken.None);
            throw;
        }
    }

    private sealed class SessionSendSpot(IZLinkSpotContext context, DispatchProbe probe)
        : IZLinkSpot<ProbeActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddHandler<SessionSendSpotHandler>("first");
            Context.Handlers.AddHandler<SessionSendSpotHandler>("second");
        }

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

        public ValueTask OnJoinedActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnDisconnectActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken
        )
        {
            probe.Events.Enqueue("disconnect");
            return ValueTask.CompletedTask;
        }
    }

    private sealed class SessionSendSpotHandler(DispatchProbe probe)
        : IZLinkSpotActorSendHandler<SessionSendSpot, ProbeActor, string>
    {
        public ValueTask HandleAsync(
            SessionSendSpot spot,
            ProbeActor actor,
            IZLinkMessageContext context,
            string message,
            CancellationToken cancellationToken
        ) => probe.HandleSendAsync(actor, message, cancellationToken);
    }

    private static Task RelaySessionSend(ZLinkSessionActor bound, string packet)
    {
        bound.Context.EnterDispatch(CreateHeader(packet) with { Codec = ZlinkStreamCodec.Json });
        try
        {
            return bound.RelayAsync(ZLinkMessage.From(packet)).AsTask();
        }
        finally
        {
            bound.Context.ExitDispatch();
        }
    }
}
