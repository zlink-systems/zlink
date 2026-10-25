using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class HostRelocationSealStageTests
{
    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task Relocated_timer_handler_mismatch_obeys_callback_commit_boundary(
        bool configure,
        bool catchMismatch
    )
    {
        using var flow = Environment.GetEnvironmentVariable("ZLINK_RELOCATION_TEST_FLOW_PATH")
            is { } path
            ? new TestHostMessageFlowListener(path + ".mismatch")
            : null;
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseJoinHandler.TrySetResult();
        trace.ReleaseTargetLifecycle.TrySetResult();
        trace.ReleaseSourceLeave.TrySetResult();
        var initialization = 0;
        ZLinkSpotActivation? failedTargetActivation = null;
        var targetFailure = new TaskCompletionSource<Exception?>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        async ValueTask RegisterAsync(
            IZLinkSpotContext context,
            CancellationToken cancellationToken
        )
        {
            if (Interlocked.Increment(ref initialization) == 1)
                await context.AddTimer<SourceTimerHandler>(
                    "pulse",
                    TimeSpan.FromHours(1),
                    cancellationToken: cancellationToken
                );
            else
            {
                if (!configure)
                    failedTargetActivation = (ZLinkSpotActivation)
                        ZLinkSpotAmbientContext.RequireCurrent();
                IZLinkTimer? restored = catchMismatch
                    ? await context.AddTimer<SourceTimerHandler>(
                        "pulse",
                        TimeSpan.FromHours(2),
                        cancellationToken: cancellationToken
                    )
                    : null;
                var snapshot = restored is Zlink.Framework.Runtime.Timers.ZLinkTimer timer
                    ? timer.Snapshot()
                    : null;
                try
                {
                    await context.AddTimer<TargetTimerHandler>(
                        "pulse",
                        TimeSpan.FromHours(1),
                        cancellationToken: cancellationToken
                    );
                    targetFailure.TrySetResult(null);
                }
                catch (Exception exception)
                {
                    if (!catchMismatch)
                    {
                        targetFailure.TrySetResult(exception);
                        throw;
                    }
                    var reattached = await context.AddTimer<SourceTimerHandler>(
                        "pulse",
                        TimeSpan.FromHours(3),
                        cancellationToken: cancellationToken
                    );
                    Assert.Same(restored, reattached);
                    Assert.Equal(
                        snapshot,
                        Assert
                            .IsType<Zlink.Framework.Runtime.Timers.ZLinkTimer>(reattached)
                            .Snapshot()
                    );
                    Assert.Equal(TimeSpan.FromHours(1), snapshot!.Period);
                    targetFailure.TrySetResult(exception);
                }
            }
        }
        if (configure)
            trace.ConfigureTarget = context =>
                RegisterAsync(context, CancellationToken.None).GetAwaiter().GetResult();
        else
            trace.InitializeTarget = RegisterAsync;
        var store = new ZLinkInMemoryProviderLocationStore();
        var roots = new SynchronizedRelocationStore();
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            true,
            relocateSpots: true
        );
        await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate("mismatched-timer", RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            true,
            relocateSpots: true
        );
        using var ready = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await Task.WhenAll(
            WaitForReadyPeersAsync(source.Services, 1, ready.Token),
            WaitForReadyPeersAsync(target.Services, 1, ready.Token)
        );
        var relocation = source
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = TimeSpan.FromSeconds(10),
                }
            )
            .AsTask();
        var failure = Assert.IsType<ZLinkFrameworkException>(
            await targetFailure.Task.WaitAsync(TimeSpan.FromSeconds(5))
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, failure.Kind);
        var result = await relocation;
        if (configure && !catchMismatch)
        {
            Assert.Equal(ZLinkFrameworkRelocationOutcome.Blocked, result.Outcome);
            Assert.Equal(ZLinkFrameworkRelocationReason.RelocationFailed, result.Reason);
            Assert.Equal(
                ZLinkFrameworkRuntimeState.Serving,
                source.Services.GetRequiredService<IZLinkFrameworkRuntime>().Status.State
            );
        }
        else
        {
            Assert.Equal(ZLinkFrameworkRelocationOutcome.Relocated, result.Outcome);
            Assert.Equal(ZLinkFrameworkRelocationReason.None, result.Reason);
        }
        if (!configure && !catchMismatch)
        {
            await target.DisposeAsync();
            Assert.NotNull(failedTargetActivation);
            Assert.True(failedTargetActivation.IsDisposed);
        }
    }

    private sealed class SourceTimerHandler : IZLinkSpotTimerHandler<BehaviorTargetSpot>
    {
        public ValueTask HandleAsync(
            BehaviorTargetSpot spot,
            ZLinkTimerTick tick,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    private sealed class TargetTimerHandler : IZLinkSpotTimerHandler<BehaviorTargetSpot>
    {
        public ValueTask HandleAsync(
            BehaviorTargetSpot spot,
            ZLinkTimerTick tick,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    [Fact]
    public void Canonical_binding_handle_survives_capture_without_private_token_bytes()
    {
        var owner = RoutingId.From(new byte[] { 0, 255, 128 });
        var session = RoutingId.From(new byte[] { 0, 254 });
        var token = ZLinkActorBoundSessionBindingToken.Canonical(
            owner,
            long.MaxValue,
            "owner",
            4,
            session,
            5
        );
        var source = new ZLinkRemoteActorBoundSessionRoute(
            owner,
            session,
            token,
            5,
            6,
            7,
            "mesh",
            8,
            9,
            long.MaxValue,
            0,
            "owner",
            4
        );
        var binding = ZLinkSessionRelocationWire.CaptureBinding(source)! with { ParticipantId = 3 };
        var participant = new ZLinkRelocationParticipantEnvelope(
            ZLinkActorAuthorityPayloadCodec.AuthorityKey("actor"),
            ZLinkPlacementObjectKind.Actor,
            6,
            7,
            default,
            [],
            []
        )
        {
            SessionBinding = binding,
        };
        var restored = ZLinkSessionRelocationWire.RestoreBinding(participant, "mesh", 8, 9);
        Assert.Equal(token, restored.BindingToken);
        Assert.Equal(source.NodeRid, restored.NodeRid);
        Assert.Equal(source.SessionRid, restored.SessionRid);
        Assert.Equal(source.SessionOwnerNodeGeneration, restored.SessionOwnerNodeGeneration);
        Assert.NotEqual(
            token,
            ZLinkActorBoundSessionBindingToken.Canonical(
                owner,
                long.MaxValue,
                "owner",
                4,
                session,
                6
            )
        );
        Assert.Throws<InvalidDataException>(() =>
            ZLinkActorBoundSessionBindingToken.Canonical(
                owner,
                ulong.MaxValue,
                "owner",
                4,
                session,
                5
            )
        );
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task Three_nodes_relocate_bound_actor_during_handler_and_stop_cleanly(
        bool bindBeforeJoin,
        bool reuseTerminalMailbox
    )
    {
        using var debug = new SealStageDebugCapture();
        using var flow = Environment.GetEnvironmentVariable("ZLINK_RELOCATION_TEST_FLOW_PATH")
            is { } path
            ? new TestHostMessageFlowListener(path)
            : null;
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseJoinHandler.TrySetResult();
        trace.ReleaseTargetLifecycle.TrySetResult();
        trace.ReleaseSourceLeave.TrySetResult();
        var store = new ZLinkInMemoryProviderLocationStore();
        var roots = new SynchronizedRelocationStore();
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            true,
            relocateSpots: true
        );
        var spots = source.Services.GetRequiredService<IZLinkSpotManager>();
        for (var i = 1; i <= 3; i++)
            _ = await spots
                .GetOrCreate($"zone-{i}", RelocationBehaviorHost.SpotType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Async();
        var actorId = $"seal-stage-{Guid.NewGuid():N}";
        _ = await source
            .Services.GetRequiredService<IZLinkActorManager>()
            .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(new BehaviorCreate(7))
            .Async();
        if (!bindBeforeJoin)
        {
            _ = await source
                .Services.GetRequiredService<IZLinkActorClient>()
                .RequestToActor(actorId, new BeginBehaviorJoin("zone-2"))
                .Async<BehaviorAck>();
            await trace.WaitAsync("publicJoinCompleted");
        }
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            true,
            relocateSpots: true
        );
        if (reuseTerminalMailbox)
        {
            var previous = target.Runtime.GetOrCreateActorState(actorId);
            var retired = previous.BeginHandlerActivationCompletion(
                previous.ClearAfterDestroyOnLane
            );
            _ = await retired.Completion;
            var failure = Assert.Throws<ZLinkFrameworkException>(() =>
                previous.ReserveHandoffRestoreBarrier()
            );
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
        }
        var builder = Host.CreateApplicationBuilder();
        var handler = new SealStageHandler();
        builder.Services.AddSingleton(handler);
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.AddRelocationStore(roots);
            options.ConfigureLocations().PollingInterval = TimeSpan.FromMilliseconds(10);
            options
                .ConfigureDispatch()
                .Diagnostics.SetLevel(
                    flow is null ? ZLinkDiagnosticsLevel.Off : ZLinkDiagnosticsLevel.Detailed
                );
            options
                .AddRouteMesh(RelocationBehaviorHost.MeshName)
                .Listen("tcp://127.0.0.1:0")
                .SetRoutingIdPrefix("aaa-session")
                .Objects()
                .Client();
            options
                .AddStreamNode("seal-stage-stream")
                .Bind("tcp://127.0.0.1:0")
                .EnableActorDispatch()
                .AddSession<SealStageSession>();
        });
        using var session = builder.Build();
        await session.StartAsync();
        using (var readyDeadline = new CancellationTokenSource(TimeSpan.FromSeconds(5)))
            await Task.WhenAll(
                WaitForReadyPeersAsync(source.Services, 2, readyDeadline.Token),
                WaitForReadyPeersAsync(target.Services, 2, readyDeadline.Token),
                WaitForReadyPeersAsync(session.Services, 2, readyDeadline.Token)
            );
        var sessionRuntime = session.Services.GetRequiredService<IZLinkFrameworkRuntime>();
        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(
                    sessionRuntime
                        .GetListenerStatus(ZLinkListenerKind.Stream, "seal-stage-stream")
                        .Endpoint!
                ),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            }
        );
        await connector.Connect.Async();
        await connector.Send(new SealStageBind(actorId)).Async();
        await handler.Bound.Task.WaitAsync(TimeSpan.FromSeconds(5));
        if (bindBeforeJoin)
        {
            await connector.Request(new BeginBehaviorJoin("zone-2")).Async<BehaviorAck>();
            await trace.WaitAsync("publicJoinCompleted");
        }
        var before = await connector
            .Request(new SealStageSessionInfoRequest())
            .Async<SealStageSessionInfo>();
        var pushed = connector.WaitFor<BehaviorAck>().Timeout(TimeSpan.FromSeconds(5)).Async();
        await connector.Send(new SealStageWork("relocation-overlap")).Async();
        await trace.WaitAsync("overlapHandlerStarted");
        var relocation = source
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = TimeSpan.FromSeconds(10),
                }
            )
            .AsTask();
        try
        {
            // The overlap handler holds its turn until relocation has started,
            // so relocation must wait for that accepted turn to complete.
            using (var relocatingDeadline = new CancellationTokenSource(TimeSpan.FromSeconds(5)))
                await WaitForStateAsync(
                    source.Services,
                    ZLinkFrameworkRuntimeState.Relocating,
                    relocatingDeadline.Token
                );
            trace.ReleaseSealStageWork.TrySetResult();
            _ = await pushed;
            Assert.Equal(
                ZLinkFrameworkRelocationOutcome.Relocated,
                (await relocation.WaitAsync(TimeSpan.FromSeconds(15))).Outcome
            );
            Assert.Equal(1, trace.TargetLifecycleAttemptCount);
            if (flow is not null)
            {
                var stages = flow.ReadLines()
                    .Select(line =>
                        System.Text.Json.JsonDocument.Parse(
                            line[(line.IndexOf(" tags=", StringComparison.Ordinal) + 6)..]
                        )
                    )
                    .ToArray();
                try
                {
                    for (var i = 1; i <= 3; i++)
                    {
                        var spotStages = stages
                            .Where(record =>
                                record.RootElement.TryGetProperty("spot_id", out var spot)
                                && spot.GetString() == $"zone-{i}"
                            )
                            .Where(record => record.RootElement.TryGetProperty("detail", out _))
                            .Select(record => record.RootElement.GetProperty("detail").GetString())
                            .ToArray();
                        foreach (
                            var transition in new[]
                            {
                                "prepare_submit",
                                "prepare_submitted",
                                "prepare_accepted",
                                "chunks_sent",
                                "chunks_assembled",
                                "target_restore_begin",
                                "target_restore_end",
                                "relay_ready_received",
                            }
                        )
                            Assert.Contains(transition, spotStages);
                    }
                }
                finally
                {
                    foreach (var stage in stages)
                        stage.Dispose();
                }
            }
            var after = await connector
                .Request(new SealStageSessionInfoRequest())
                .Async<SealStageSessionInfo>();
            Assert.Equal(before, after);
            await connector.Send(new SealStageWork("after-relocation")).Async();
            await trace.WaitAsync("afterRelocationPush");
            Assert.False(handler.Disconnected.Task.IsCompleted);
            await connector.Close.Async();
            Assert.Equal(
                ZLinkFrameworkTerminationOutcome.Stopped,
                (
                    await target
                        .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                        .ShutdownAsync(TimeSpan.FromSeconds(5))
                ).Outcome
            );
        }
        finally
        {
            trace.ReleaseSealStageWork.TrySetResult();
            await session.StopAsync();
        }
    }

    // Relocation 04 §4.2, §4.4, §4.6, §7 and §10 (#1877): a member frame that
    // the source holds after the SpotWide seal reaches the target and runs
    // there after the target commit. Held before the payload is captured, it
    // is carried as backlog; held after the payload, it follows through the
    // Message Follow route from the relocating (Draining but still admitted)
    // source. Either way the handler pushes through the bound Session with the
    // committed target binding.
    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task Held_member_work_replayed_on_target_pushes_through_bound_session(
        bool heldAfterPayload,
        bool holdTargetCommitResponse
    )
    {
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseSealStageWork.TrySetResult();
        var holdGate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        CommitResponseGateStore? commitStore = null;
        if (heldAfterPayload)
            trace.TargetActorRestoreGate = holdGate.Task;
        else
            trace.SourceActorCaptureGate = holdGate.Task;
        await RunBoundMemberRelocationAsync(
            trace,
            async context =>
            {
                var relocation = context.StartRelocation();
                // The source capture (or the target restore) waits after the
                // SpotWide seal, so the source holds member ingress. The
                // Session owner already holds Session→Actor messages for this
                // binding and the relocating source admits no new local
                // operation, so the held frame comes from another node's Actor
                // client.
                await trace.WaitAsync(
                    heldAfterPayload ? "targetStateRestoring" : "sourceStateCapturing"
                );
                await context.SendFromSessionNodeAsync("held-work");
                await context
                    .Backlog.WaitForBacklogAsync(context.ActorId)
                    .WaitAsync(TimeSpan.FromSeconds(10));
                holdGate.TrySetResult();
                try
                {
                    if (commitStore is not null)
                        await commitStore.Committed.Task.WaitAsync(TimeSpan.FromSeconds(10));
                    Assert.Equal(
                        ZLinkFrameworkRelocationOutcome.Relocated,
                        (await relocation.WaitAsync(TimeSpan.FromSeconds(15))).Outcome
                    );
                    if (commitStore is not null)
                        Assert.DoesNotContain("overlapHandlerStarted", trace.Events);
                }
                finally
                {
                    commitStore?.Release.TrySetResult();
                }
                await trace.WaitAsync("sealStagePushed:held-work@target");
            },
            holdTargetCommitResponse
                ? (inner, actorId, sourceRid) =>
                    commitStore = new CommitResponseGateStore(inner, actorId, sourceRid)
                : null
        );
    }

    // Relocation 04 §4.1 (#1877): the Session owner seals a member Actor's
    // binding before the source stops that member's application dispatch.
    // While a member turn holds the SpotWide source, the source already
    // captures member ingress; by then a Session→Actor message is held by the
    // Session owner and reaches the Actor on the target. It never enters the
    // source handoff backlog through the bound route.
    [Fact]
    public async Task Member_session_binding_is_sealed_before_source_dispatch_stops()
    {
        var trace = new RelocationBehaviorTrace();
        await RunBoundMemberRelocationAsync(
            trace,
            async context =>
            {
                await context.SendFromSessionNodeAsync("hold-turn");
                await trace.WaitAsync("overlapHandlerStarted");
                var relocation = context.StartRelocation();
                var sourceState = context.Source.Runtime.GetOrCreateActorState(context.ActorId);
                await WaitUntilAsync(() => sourceState.Handoff.CapturesSourceIngress);
                await context.Connector.Send(new SealStageWork("via-session")).Async();
                trace.ReleaseSealStageWork.TrySetResult();
                Assert.Equal(
                    ZLinkFrameworkRelocationOutcome.Relocated,
                    (await relocation.WaitAsync(TimeSpan.FromSeconds(15))).Outcome
                );
                await trace.WaitAsync("sealStagePushed:via-session@target");
                Assert.DoesNotContain(
                    context.Backlog.Lines,
                    line =>
                        line.StartsWith(
                            $"capture_entry actor={context.ActorId} ",
                            StringComparison.Ordinal
                        ) && line.Contains(" bound_route=True ", StringComparison.Ordinal)
                );
            }
        );
    }

    private sealed record BoundMemberRelocationContext(
        RelocationBehaviorHost Source,
        IServiceProvider SessionServices,
        IZlinkStreamConnector Connector,
        SpotWideMemberSealCaptureTests.HandoffBacklogLoggerProvider Backlog,
        string ActorId
    )
    {
        internal Task<ZLinkFrameworkRelocationResult> StartRelocation() =>
            Source
                .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                .RelocateAsync(
                    new ZLinkFrameworkRelocationOptions
                    {
                        Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                        Deadline = TimeSpan.FromSeconds(10),
                    }
                )
                .AsTask();

        internal async Task SendFromSessionNodeAsync(string marker) =>
            await SessionServices
                .GetRequiredService<IZLinkActorClient>()
                .SendToActor(ActorId, new SealStageWork(marker))
                .Async();
    }

    // Source, target and a Session node with one Session-bound member Actor of
    // the SpotWide Spot "zone-2" on the source.
    private static async Task RunBoundMemberRelocationAsync(
        RelocationBehaviorTrace trace,
        Func<BoundMemberRelocationContext, Task> scenario,
        Func<IZLinkLocationStore, string, RoutingId, IZLinkLocationStore>? targetStore = null
    )
    {
        using var debug = new SealStageDebugCapture();
        using var flow = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } path
            ? new TestHostMessageFlowListener(path + $".bound-member-{Guid.NewGuid():N}")
            : null;
        trace.ReleaseJoinHandler.TrySetResult();
        trace.ReleaseTargetLifecycle.TrySetResult();
        trace.ReleaseSourceLeave.TrySetResult();
        var backlog = new SpotWideMemberSealCaptureTests.HandoffBacklogLoggerProvider();
        var store = new ZLinkInMemoryProviderLocationStore();
        var roots = new SynchronizedRelocationStore();
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            true,
            relocateSpots: true,
            configureServices: services =>
                services.AddLogging(logging => logging.AddProvider(backlog))
        );
        _ = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate("zone-2", RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        var actorId = $"bound-member-{Guid.NewGuid():N}";
        _ = await source
            .Services.GetRequiredService<IZLinkActorManager>()
            .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(new BehaviorCreate(7))
            .Async();
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            targetStore?.Invoke(store, actorId, source.LocalNodeRid) ?? store,
            roots,
            true,
            relocateSpots: true
        );
        var builder = Host.CreateApplicationBuilder();
        var handler = new SealStageHandler();
        builder.Services.AddSingleton(handler);
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.AddRelocationStore(roots);
            options.ConfigureLocations().PollingInterval = TimeSpan.FromMilliseconds(10);
            options
                .AddRouteMesh(RelocationBehaviorHost.MeshName)
                .Listen("tcp://127.0.0.1:0")
                .SetRoutingIdPrefix("aaa-session")
                .Objects()
                .Client();
            options
                .ConfigureDispatch()
                .Diagnostics.SetLevel(
                    flow is null ? ZLinkDiagnosticsLevel.Off : ZLinkDiagnosticsLevel.Detailed
                );
            options
                .AddStreamNode("bound-member-stream")
                .Bind("tcp://127.0.0.1:0")
                .EnableActorDispatch()
                .AddSession<SealStageSession>();
        });
        using var session = builder.Build();
        await session.StartAsync();
        try
        {
            using (var readyDeadline = new CancellationTokenSource(TimeSpan.FromSeconds(5)))
                await Task.WhenAll(
                    WaitForReadyPeersAsync(source.Services, 2, readyDeadline.Token),
                    WaitForReadyPeersAsync(target.Services, 2, readyDeadline.Token),
                    WaitForReadyPeersAsync(session.Services, 2, readyDeadline.Token)
                );
            await using var connector = ZlinkStreamConnectorFactory.Create(
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri(
                        session
                            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                            .GetListenerStatus(ZLinkListenerKind.Stream, "bound-member-stream")
                            .Endpoint!
                    ),
                    DispatchMode = ZlinkStreamDispatchMode.Immediate,
                    Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                    Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                }
            );
            await connector.Connect.Async();
            await connector.Send(new SealStageBind(actorId)).Async();
            await handler.Bound.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await connector.Request(new BeginBehaviorJoin("zone-2")).Async<BehaviorAck>();
            await trace.WaitAsync("publicJoinCompleted");
            await scenario(
                new BoundMemberRelocationContext(
                    source,
                    session.Services,
                    connector,
                    backlog,
                    actorId
                )
            );
            Assert.False(handler.Disconnected.Task.IsCompleted);
            await connector.Close.Async();
        }
        finally
        {
            trace.ReleaseSealStageWork.TrySetResult();
            await session.StopAsync();
        }
    }

    // Keep the committed Store state visible to the source while the target
    // has not received the commit response or refreshed its local authority.
    private sealed class CommitResponseGateStore(
        IZLinkLocationStore inner,
        string actorId,
        RoutingId sourceRid
    ) : IZLinkLocationStore
    {
        private readonly ZLinkProviderLocationRepository reader = new(inner);

        internal TaskCompletionSource Committed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        ) => inner.ReadAsync(key, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            var result = await inner.WriteAsync(request, cancellationToken);
            if (result is ZLinkStoreWriteResult.Applied && !Release.Task.IsCompleted)
            {
                var authority = await reader.ReadAuthorityAsync(
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId),
                    cancellationToken
                );
                if (
                    authority is ZLinkAuthorityReadResult.Found found
                    && found.Snapshot.Allocation.Descriptor.Rid != sourceRid
                )
                {
                    Committed.TrySetResult();
                    await Release.Task.WaitAsync(cancellationToken);
                }
            }
            return result;
        }
    }

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        while (!condition())
            await Task.Delay(TimeSpan.FromMilliseconds(5), deadline.Token);
    }

    private static async Task WaitForReadyPeersAsync(
        IServiceProvider services,
        int count,
        CancellationToken cancellationToken
    )
    {
        await foreach (
            var observed in services
                .GetRequiredService<IZLinkRouteMeshRuntime>()
                .ObserveAsync(RelocationBehaviorHost.MeshName, cancellationToken)
        )
            if (observed.Status.ReadyPeerCount == count)
                return;
    }

    private static async Task WaitForStateAsync(
        IServiceProvider services,
        ZLinkFrameworkRuntimeState state,
        CancellationToken cancellationToken
    )
    {
        await foreach (
            var observed in services
                .GetRequiredService<IZLinkFrameworkRuntime>()
                .ObserveAsync(cancellationToken)
        )
            if (observed.Status.State == state)
                return;
    }
}

internal sealed record SealStageBind(string ActorId);

internal sealed record SealStageSessionInfoRequest;

internal sealed record SealStageSessionInfo(string SessionId, string ActorId, ulong Generation);

internal sealed record SealStageWork(string Marker);

internal sealed class SealStagePushHandler(RelocationBehaviorTrace trace, BehaviorNode node)
    : IZLinkSpotActorSendHandler<BehaviorTargetSpot, BehaviorActor, SealStageWork>
{
    public async ValueTask HandleAsync(
        BehaviorTargetSpot spot,
        BehaviorActor actor,
        IZLinkMessageContext context,
        SealStageWork request,
        CancellationToken cancellationToken
    )
    {
        trace.Record("overlapHandlerStarted");
        await trace.ReleaseSealStageWork.Task.WaitAsync(cancellationToken);
        try
        {
            await actor.Context.BoundSession.Send(new BehaviorAck(request.Marker)).Async();
        }
        catch (Exception exception)
        {
            trace.Record($"sealStagePushFailed:{request.Marker}@{node.Name}:{exception.Message}");
            throw;
        }
        trace.Record($"sealStagePushed:{request.Marker}@{node.Name}");
        if (request.Marker == "after-relocation")
            trace.Record("afterRelocationPush");
    }
}

internal sealed class SealStageDebugCapture : IDisposable
{
    private readonly TextWriter original = Console.Error;
    private readonly StreamWriter? writer;

    internal SealStageDebugCapture()
    {
        if (Environment.GetEnvironmentVariable("ZLINK_RELOCATION_TEST_FLOW_PATH") is { } path)
        {
            writer = new StreamWriter(path + ".spot", append: true) { AutoFlush = true };
            Console.SetError(TextWriter.Synchronized(writer));
        }
    }

    public void Dispose()
    {
        Console.SetError(original);
        writer?.Dispose();
    }
}

internal sealed class SealStageHandler
{
    internal TaskCompletionSource Bound { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    internal TaskCompletionSource Disconnected { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}

internal sealed class SealStageSession(
    IZLinkSessionContext context,
    IZLinkActorManager actors,
    SealStageHandler handler
) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        handler.Disconnected.TrySetResult();
        return ValueTask.CompletedTask;
    }

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken
    )
    {
        if (dispatch.PacketName == nameof(SealStageBind))
        {
            var request = payload.Decode<SealStageBind>();
            var result = await actors
                .GetOrCreate(request.ActorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Async(cancellationToken);
            var actor = Assert.IsType<ZLinkActorCreateResult.Existing>(result).Actor;
            await Context.Actors.BindOrGetAsync(actor, cancellationToken);
            handler.Bound.TrySetResult();
            return;
        }
        if (dispatch.PacketName == nameof(SealStageSessionInfoRequest))
        {
            var actor = Context.Actors.Bound.Single().Ref;
            await Context
                .Client.Reply(
                    new SealStageSessionInfo(
                        Context.SessionId,
                        actor.ActorId,
                        actor.ObjectGeneration
                    )
                )
                .Async();
            return;
        }
        await Context.Actors.Bound.Single().RelayAsync(payload);
    }
}
