using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class JoinCallbackSendTests
{
    [Fact]
    public async Task Preserve_after_relay_readiness_releases_source_session_seal_without_completion()
    {
        using var flowListener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseJoinHandler.SetResult();
        trace.ReleaseSourceLeave.SetResult();
        trace.ReleaseTargetLifecycle.SetResult();
        var transport = new CanonicalRelocationTransportProbe(trace);
        transport.ReleasePrepareCall.SetResult();
        var store = new RecordingLocationStore(new ZLinkInMemoryProviderLocationStore(), trace);
        var relocationStore = new SynchronizedRelocationStore();
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            relocationStore,
            registerTargetSpot: false,
            canonicalTransportProbe: transport
        );
        var actorId = $"join-preserve-session-{Guid.NewGuid():N}";
        trace.ActorId = actorId;
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await source
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Async()
        );
        var session = new ZLinkSessionContext(
            source.Runtime,
            new RelocationBehaviorStream(RoutingId.From("preserved-session")),
            new RelocationBehaviorSessionHandlers(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask
        );
        await session.Actors.BindAsync(created.Actor);
        Assert.True(source.Runtime.TryGetActorState(actorId, out var state));
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            relocationStore,
            registerTargetSpot: true,
            canonicalTransportProbe: transport
        );
        var spot = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate($"preserved-spot-{Guid.NewGuid():N}", RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);
        try
        {
            _ = await source
                .Services.GetRequiredService<IZLinkActorClient>()
                .RequestToActor(
                    actorId,
                    new BeginBehaviorJoin(spot.Spot.SpotId, TimeSpan.FromSeconds(2))
                )
                .Async<BehaviorAck>();
            await transport.CutoverSendStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var handoffId = transport.RelocationId.ToString("N");
            Assert.True(state.TryGetSourceSessionRelocation(handoffId, out _));
            // Keep CUTOVER pending until this Join's deadline makes the source
            // execute its own Preserve fence. A foreign Preserve is not its terminal.
            var barrier = state.ReserveHandoffRestoreBarrier();
            using var drained = await barrier
                .ClaimAsync()
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5));
            Assert.DoesNotContain(trace.JoinCompletions, completion => completion.Node == "source");
            Assert.False(state.TryGetSourceSessionRelocation(handoffId, out _));
        }
        finally
        {
            transport.ReleaseCutoverSend.TrySetResult();
        }
    }

    [Fact]
    public async Task Source_settlement_after_target_commit_and_destroy_has_no_public_failure()
    {
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseJoinHandler.SetResult();
        trace.ReleaseSourceLeave.SetResult();
        trace.ReleaseTargetLifecycle.SetResult();
        var store = new RecordingLocationStore(new ZLinkInMemoryProviderLocationStore(), trace);
        var relocationStore = new SynchronizedRelocationStore();
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            relocationStore,
            registerTargetSpot: false,
            pollingInterval: TimeSpan.FromSeconds(1)
        );
        var actorId = $"join-deleted-target-{Guid.NewGuid():N}";
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await source
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Async()
        );
        Assert.True(source.Runtime.TryGetActorState(actorId, out var sourceState));
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            relocationStore,
            registerTargetSpot: true
        );
        var spot = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(
                $"join-deleted-target-spot-{Guid.NewGuid():N}",
                RelocationBehaviorHost.SpotType
            )
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        Assert.NotEqual(created.Actor.NodeRid, spot.Spot.NodeRid);
        _ = await source
            .Services.GetRequiredService<IZLinkActorClient>()
            .RequestToActor(actorId, new BeginBehaviorJoin(spot.Spot.SpotId))
            .Async<BehaviorAck>();
        await trace.WaitAsync("publicJoinCompleted");
        await trace.WaitAsync("targetLifecycleCompleted");
        Assert.True(target.Runtime.TryGetActorState(actorId, out var targetState));
        _ = await target.Runtime.JoinActorEntrySpotAsync(
            Assert.IsType<BehaviorActor>(targetState.Actor),
            ZLinkMessage.Empty,
            operationId: null,
            CancellationToken.None
        );
        var manager = target.Services.GetRequiredService<IZLinkActorManager>();
        var current = Assert.NotNull(await manager.FindAsync(actorId));
        Assert.True(await manager.DestroyAsync(current));
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await target
                .Runtime.Registration.Locations.ResolveStore()!
                .ReadAuthorityAsync(ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId))
        );
        var barrier = sourceState.ReserveHandoffRestoreBarrier();
        using var drained = await barrier.ClaimAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        var completion = Assert.Single(trace.JoinCompletions);
        Assert.Equal("target", completion.Node);
        Assert.IsType<ZLinkActorJoinCompletion.Accepted>(completion.Completion);
    }

    [Fact]
    public async Task Cross_node_join_tracks_committed_authority_before_target_lifecycle()
    {
        using var flowListener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseJoinHandler.SetResult();
        trace.ReleaseSourceLeave.SetResult();
        trace.ReleaseTargetLifecycle.SetResult();
        var locationStore = new RecordingLocationStore(
            new ZLinkInMemoryProviderLocationStore(),
            trace
        );
        var relocationStore = new SynchronizedRelocationStore();
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: false
        );
        var actorId = $"join-owner-actor-{Guid.NewGuid():N}";
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await source
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Async()
        );
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: true
        );
        var ownershipUpdate = new TaskCompletionSource<Exception?>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var completionObserved = new TaskCompletionSource<Exception?>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        trace.OnPublicJoinCompleted = async actor =>
        {
            var failure = await Record.ExceptionAsync(async () =>
            {
                var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                    await target
                        .Runtime.Registration.Locations.ResolveStore()!
                        .ReadAuthorityAsync(
                            ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId)
                        )
                );
                Assert.False(
                    ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                        authority.Snapshot.Payload.Span,
                        out _
                    )
                );
                Assert.True(target.Runtime.TryGetActorState(actor.ActorId, out var state));
                Assert.False(state.Handoff.TargetActivation.IsCompleted);
                Assert.Empty(trace.DeliveredMarkers);
            });
            completionObserved.SetResult(failure);
        };
        trace.OnTargetJoined = async actor =>
        {
            var failure = await Record.ExceptionAsync(async () =>
            {
                Assert.True(target.Runtime.TryGetActorState(actor.ActorId, out var state));
                var actorRef = Assert.IsType<ZLinkBackendActorRef>(state.NativeActorRef);
                await target.Runtime.LocationLifecycle!.ActorOwnership.PublishActorRefAsync(
                    state.RuntimeActorId,
                    actorRef.ToNative(RelocationBehaviorHost.MeshName)
                );
            });
            ownershipUpdate.SetResult(failure);
        };
        var spot = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate($"join-owner-spot-{Guid.NewGuid():N}", RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        Assert.NotEqual(created.Actor.NodeRid, spot.Spot.NodeRid);
        _ = await source
            .Services.GetRequiredService<IZLinkActorClient>()
            .RequestToActor(actorId, new BeginBehaviorJoin(spot.Spot.SpotId))
            .Async<BehaviorAck>();
        await trace.WaitAsync("publicJoinCompleted");

        Assert.True(ownershipUpdate.Task.IsCompleted);
        Assert.Null(await ownershipUpdate.Task);
        Assert.Null(await completionObserved.Task.WaitAsync(TimeSpan.FromSeconds(5)));
    }

    [Fact]
    public async Task Ordinary_local_send_is_accepted_and_delivered_without_join()
    {
        const int iterations = 1024;
        const int warmup = iterations;
        var trace = new RelocationBehaviorTrace
        {
            OrdinarySendBatchSize = iterations,
            ConfigureEntry = context =>
                context.Handlers.AddActorPacket<OrdinaryLocalSendHandler, BehaviorActor>(
                    nameof(OrdinaryLocalSend)
                ),
        };
        await using var host = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            new ZLinkInMemoryProviderLocationStore(),
            new SynchronizedRelocationStore(),
            registerTargetSpot: false
        );
        host.Runtime.Registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Off);
        var actorId = $"ordinary-send-{Guid.NewGuid():N}";
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await host
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async()
        );
        Assert.Equal(host.LocalNodeRid, created.Actor.NodeRid);
        var client = host.Services.GetRequiredService<IZLinkActorClient>();
        var message = new OrdinaryLocalSend();
        for (var index = 0; index < warmup; index++)
            await client.SendToActor(actorId, message).Async();
        await trace.WaitAsync($"{RelocationBehaviorTrace.OrdinarySendBatchEventPrefix}{warmup}");
        Assert.Equal(warmup, Volatile.Read(ref trace.OrdinarySendCount));

        var submitThread = Environment.CurrentManagedThreadId;
        var totalBefore = GC.GetTotalAllocatedBytes(precise: true);
        long submitBytes = 0;
        var incomplete = 0;
        var callerThreadHops = 0;
        for (var index = 0; index < iterations; index++)
        {
            var callerThread = Environment.CurrentManagedThreadId;
            var threadBefore = GC.GetAllocatedBytesForCurrentThread();
            var send = client.SendToActor(actorId, message).Async();
            submitBytes += GC.GetAllocatedBytesForCurrentThread() - threadBefore;
            if (!send.IsCompletedSuccessfully)
                incomplete++;
            await send;
            if (callerThread != Environment.CurrentManagedThreadId)
                callerThreadHops++;
        }
        var sameSubmitThread = submitThread == Environment.CurrentManagedThreadId;
        await trace.WaitAsync(
            $"{RelocationBehaviorTrace.OrdinarySendBatchEventPrefix}{warmup + iterations}"
        );
        var totalBytes = GC.GetTotalAllocatedBytes(precise: true) - totalBefore;
        Assert.Equal(warmup + iterations, Volatile.Read(ref trace.OrdinarySendCount));
        Assert.Empty(trace.Events.Where(value => value.Contains("Join", StringComparison.Ordinal)));
        if (Environment.GetEnvironmentVariable("ZLINK_LOCAL_SEND_METRICS_PATH") is { } path)
            await File.WriteAllTextAsync(
                path,
                $"iterations={iterations} submitBytesPerMessage={(double)submitBytes / iterations} totalBytesPerMessage={(double)totalBytes / iterations} incomplete={incomplete} callerThreadHops={callerThreadHops} sameSubmitThread={sameSubmitThread}\n"
            );
        if (Environment.GetEnvironmentVariable("ZLINK_LOCAL_SEND_SUBMIT_BASELINE") is { } baseline)
            Assert.True(
                (double)submitBytes / iterations
                    <= double.Parse(baseline, System.Globalization.CultureInfo.InvariantCulture),
                $"Submit allocated {(double)submitBytes / iterations} B/message; baseline {baseline}."
            );
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task OnJoinedActor_send_is_delivered_after_join_completion(bool crossNode)
    {
        using var taskLog = Environment.GetEnvironmentVariable(
            "ZLINK_RELOCATION_TEST_TASK_LOG_PATH"
        )
            is { } taskLogPath
            ? File.CreateText($"{taskLogPath}.{(crossNode ? "cross" : "local")}")
            : null;
        var originalError = Console.Error;
        try
        {
            if (taskLog is not null)
                Console.SetError(TextWriter.Synchronized(taskLog));
            using var flowListener = Environment.GetEnvironmentVariable(
                "ZLINK_RELOCATION_TEST_FLOW_PATH"
            )
                is { } flowPath
                ? new TestHostMessageFlowListener($"{flowPath}.{(crossNode ? "cross" : "local")}")
                : null;
            var trace = new RelocationBehaviorTrace
            {
                ConfigureTarget = context =>
                    context.Handlers.AddActorPacket<JoinCallbackSendHandler, BehaviorActor>(
                        nameof(JoinCallbackSend)
                    ),
            };
            trace.ReleaseJoinHandler.SetResult();
            trace.ReleaseSourceLeave.SetResult();
            trace.ReleaseTargetLifecycle.SetResult();
            var locationStore = new RecordingLocationStore(
                new ZLinkInMemoryProviderLocationStore(),
                trace
            );
            var relocationStore = new SynchronizedRelocationStore();
            await using var source = await RelocationBehaviorHost.StartAsync(
                "source",
                trace,
                locationStore,
                relocationStore,
                registerTargetSpot: !crossNode
            );
            var actorId = $"join-callback-actor-{Guid.NewGuid():N}";
            var created = Assert.IsType<ZLinkActorCreateResult.Created>(
                await source
                    .Services.GetRequiredService<IZLinkActorManager>()
                    .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                    .InMesh(RelocationBehaviorHost.MeshName)
                    .Request(new BehaviorCreate(7))
                    .Timeout(TimeSpan.FromSeconds(10))
                    .Async()
            );
            await using var remoteTarget = crossNode
                ? await RelocationBehaviorHost.StartAsync(
                    "target",
                    trace,
                    locationStore,
                    relocationStore,
                    registerTargetSpot: true
                )
                : null;
            var target = remoteTarget ?? source;
            var targetClient = target.Services.GetRequiredService<IZLinkActorClient>();
            trace.OnTargetJoined = async actor =>
            {
                await targetClient.SendToActor(actor.ActorId, new JoinCallbackSend()).Async();
                trace.Record("joinedSendSubmitted");
            };
            var spot = await source
                .Services.GetRequiredService<IZLinkSpotManager>()
                .GetOrCreate(
                    $"join-callback-spot-{Guid.NewGuid():N}",
                    RelocationBehaviorHost.SpotType
                )
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(ZLinkMessage.Empty)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async();
            Assert.Equal(crossNode, created.Actor.NodeRid != spot.Spot.NodeRid);
            _ = await source
                .Services.GetRequiredService<IZLinkActorClient>()
                .RequestToActor(actorId, new BeginBehaviorJoin(spot.Spot.SpotId))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async<BehaviorAck>();
            await trace.WaitAsync("joinedSendSubmitted");
            await trace.WaitAsync("publicJoinCompleted");
            await trace.WaitAsync("joinedSendDelivered");
        }
        finally
        {
            if (taskLog is not null)
                Console.SetError(originalError);
        }
    }
}

internal sealed record JoinCallbackSend;

internal sealed record OrdinaryLocalSend;

internal sealed class OrdinaryLocalSendHandler(RelocationBehaviorTrace trace)
    : IZLinkEntrySpotActorSendHandler<BehaviorEntrySpot, BehaviorActor, OrdinaryLocalSend>
{
    public ValueTask HandleAsync(
        BehaviorEntrySpot spot,
        BehaviorActor actor,
        IZLinkMessageContext context,
        OrdinaryLocalSend message,
        CancellationToken cancellationToken
    )
    {
        var count = Interlocked.Increment(ref trace.OrdinarySendCount);
        if (count % trace.OrdinarySendBatchSize == 0)
            trace.Record($"{RelocationBehaviorTrace.OrdinarySendBatchEventPrefix}{count}");
        return ValueTask.CompletedTask;
    }
}

internal sealed class JoinCallbackSendHandler(RelocationBehaviorTrace trace)
    : IZLinkSpotActorSendHandler<BehaviorTargetSpot, BehaviorActor, JoinCallbackSend>
{
    public ValueTask HandleAsync(
        BehaviorTargetSpot spot,
        BehaviorActor actor,
        IZLinkMessageContext context,
        JoinCallbackSend message,
        CancellationToken cancellationToken
    )
    {
        Assert.Contains("publicJoinCompleted", trace.Events);
        trace.Record("joinedSendDelivered");
        return ValueTask.CompletedTask;
    }
}
