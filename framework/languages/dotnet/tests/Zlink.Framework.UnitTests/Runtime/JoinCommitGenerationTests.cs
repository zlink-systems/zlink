using Microsoft.Extensions.DependencyInjection;
using Xunit.Abstractions;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class JoinCommitGenerationTests(ITestOutputHelper output)
{
    [Theory]
    [InlineData(false, 0)]
    [InlineData(false, 1)]
    [InlineData(false, 2)]
    [InlineData(true, 0)]
    [InlineData(true, 1)]
    [InlineData(true, 2)]
    public async Task Membership_uses_activation_incarnation_and_store_authority(
        bool restore,
        int mode
    )
    {
        ZLinkSpotActivation? activation = null;
        var trace = new RelocationBehaviorTrace
        {
            InitializeTarget = (context, _) =>
            {
                activation = (ZLinkSpotActivation)context;
                return ValueTask.CompletedTask;
            },
        };
        trace.ReleaseJoinHandler.SetResult();
        trace.ReleaseSourceLeave.SetResult();
        trace.ReleaseTargetLifecycle.SetResult();
        var store = new RecordingLocationStore(new ZLinkInMemoryProviderLocationStore(), trace);
        await using var host = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            new SynchronizedRelocationStore(),
            registerTargetSpot: true
        );
        var actorId = $"generation-actor-{Guid.NewGuid():N}";
        _ = await host
            .Services.GetRequiredService<IZLinkActorManager>()
            .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(new BehaviorCreate(7))
            .Async();
        _ = await host
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate($"generation-spot-{Guid.NewGuid():N}", RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        Assert.NotNull(activation);
        Assert.True(host.Runtime.TryGetActorState(actorId, out var state));
        var locations = host.Runtime.LocationLifecycle!.SpotLocations;
        var generation = await locations.GetTrackedGenerationAsync(activation.RuntimeSpotId);
        Assert.Equal(activation.ObjectGeneration, generation);
        var repository = new ZLinkProviderLocationRepository(store);
        var actorKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var before = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await repository.ReadAuthorityAsync(actorKey)
        );
        if (mode == 1)
            await locations.ForgetTrackedAsync(activation.RuntimeSpotId, generation!.Value);
        if (mode == 2)
            await locations.ReleaseAsync(
                ZLinkMeshName.FromBoundary(RelocationBehaviorHost.MeshName, "meshName"),
                activation.RuntimeSpotId
            );
        Assert.Equal(
            mode == 0 ? generation : null,
            await locations.GetTrackedGenerationAsync(activation.RuntimeSpotId)
        );
        trace.OnTargetJoined = async _ =>
        {
            var observed = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(actorKey)
            );
            if (restore)
                Assert.Equal(
                    before.Snapshot.Payload.ToArray(),
                    observed.Snapshot.Payload.ToArray()
                );
            else
            {
                Assert.True(
                    ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                        observed.Snapshot.Payload.Span,
                        out var member
                    )
                );
                Assert.Equal(generation!.Value, member.CurrentSpotGeneration);
            }
        };
        var error = await Record.ExceptionAsync(async () =>
        {
            if (restore)
                await activation.RestoreActorAfterFailedHandoffAsync(
                    state.Actor!,
                    CancellationToken.None
                );
            else
                await activation.JoinActorAsync(
                    state.Actor!,
                    ZLinkMessage.Empty,
                    CancellationToken.None
                );
        });
        output.WriteLine(
            $"restore={restore} mode={mode} generation={generation} result={error?.ToString() ?? "SUCCESS"}"
        );
        var after = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await repository.ReadAuthorityAsync(actorKey)
        );
        if (mode == 2)
        {
            Assert.Equal(
                ZLinkFrameworkErrorKind.NotFound,
                Assert.IsType<ZLinkFrameworkException>(error).Kind
            );
            Assert.Equal(before.Snapshot.Payload.ToArray(), after.Snapshot.Payload.ToArray());
        }
        else
        {
            Assert.Null(error);
            Assert.True(
                ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                    after.Snapshot.Payload.Span,
                    out var payload
                )
            );
            Assert.Equal(activation.SpotId, payload.CurrentSpotId);
            Assert.Equal(generation!.Value, payload.CurrentSpotGeneration);
            Assert.Equal(1, trace.TargetLifecycleAttemptCount);
        }
    }

    [Theory]
    [InlineData(false, 3)]
    [InlineData(true, 3)]
    [InlineData(false, 0)]
    [InlineData(false, 1)]
    [InlineData(false, 2)]
    [InlineData(true, 0)]
    [InlineData(true, 1)]
    [InlineData(true, 2)]
    public async Task Deferred_join_classifies_store_authority_after_admission(
        bool crossNode,
        int mode
    )
    {
        using var listener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener($"{flowPath}.{crossNode}.{mode}")
            : null;
        ZLinkSpotActivation? activation = null;
        var trace = new RelocationBehaviorTrace
        {
            ExpectedJoinAccepted = mode < 2,
            InitializeTarget = (context, _) =>
            {
                activation = (ZLinkSpotActivation)context;
                return ValueTask.CompletedTask;
            },
        };
        trace.ReleaseJoinHandler.SetResult();
        trace.ReleaseSourceLeave.SetResult();
        trace.ReleaseTargetLifecycle.SetResult();
        var store = new RecordingLocationStore(new ZLinkInMemoryProviderLocationStore(), trace);
        var roots = new SynchronizedRelocationStore();
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            registerTargetSpot: !crossNode
        );
        var actorId = $"deferred-generation-{Guid.NewGuid():N}";
        _ = await source
            .Services.GetRequiredService<IZLinkActorManager>()
            .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(new BehaviorCreate(7))
            .Async();
        await using var remoteTarget = crossNode
            ? await RelocationBehaviorHost.StartAsync(
                "target",
                trace,
                store,
                roots,
                registerTargetSpot: true
            )
            : null;
        var target = remoteTarget ?? source;
        var spot = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate($"deferred-spot-{Guid.NewGuid():N}", RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        Assert.NotNull(activation);
        var generation = activation.ObjectGeneration;
        trace.BeforeTargetAdmissionReply = async () =>
        {
            var locations = target.Runtime.LocationLifecycle!.SpotLocations;
            Assert.Equal(
                generation,
                await locations.GetTrackedGenerationAsync(activation.RuntimeSpotId)
            );
            if (mode == 1)
                await locations.ForgetTrackedAsync(activation.RuntimeSpotId, generation);
            if (mode == 2)
                await locations.ReleaseAsync(
                    ZLinkMeshName.FromBoundary(RelocationBehaviorHost.MeshName, "meshName"),
                    activation.RuntimeSpotId
                );
            if (mode == 3)
            {
                var repository = new ZLinkProviderLocationRepository(store);
                var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(activation.SpotId);
                var current = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                    await repository.ReadAuthorityAsync(key)
                );
                var replacement = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
                    await repository.CompareExchangeAuthorityAsync(
                        key,
                        current.Snapshot.StoreVersion,
                        new ZLinkAuthorityMutation.Put(
                            current.Snapshot.Payload,
                            ZLinkAuthorityGenerationTransition.Reincarnate,
                            null,
                            null
                        )
                    )
                );
                Assert.NotEqual(generation, replacement.Snapshot.ObjectGeneration);
            }
            var spotAuthority = await new ZLinkProviderLocationRepository(store).ReadAuthorityAsync(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(activation.SpotId)
            );
            output.WriteLine(
                $"admission_reply mode={mode} spot={activation.SpotId} generation={generation} authority={spotAuthority.GetType().Name}"
            );
            if (mode == 2)
                Assert.IsType<ZLinkAuthorityReadResult.Missing>(spotAuthority);
            trace.BeforeTargetAdmissionReply = null;
        };
        _ = await source
            .Services.GetRequiredService<IZLinkActorClient>()
            .RequestToActor(actorId, new BeginBehaviorJoin(spot.Spot.SpotId))
            .Async<BehaviorAck>();
        try
        {
            await trace.WaitAsync("publicJoinCompleted");
        }
        finally
        {
            foreach (var observed in trace.JoinCompletions)
                output.WriteLine($"observed={observed}");
        }
        var completion = Assert.Single(trace.JoinCompletions).Completion;
        output.WriteLine(
            $"crossNode={crossNode} mode={mode} generation={generation} completion={completion}"
        );
        if (mode >= 2)
        {
            Assert.Equal(
                mode == 2 ? ZLinkFrameworkErrorKind.NotFound : ZLinkFrameworkErrorKind.Unavailable,
                Assert.IsType<ZLinkActorJoinCompletion.Failed>(completion).Kind
            );
            Assert.Equal(0, trace.TargetLifecycleAttemptCount);
            var retained = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await new ZLinkProviderLocationRepository(store).ReadAuthorityAsync(
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId)
                )
            );
            Assert.Equal(source.LocalNodeRid, retained.Snapshot.Allocation.Descriptor.Rid);
        }
        else
        {
            Assert.IsType<ZLinkActorJoinCompletion.Accepted>(completion);
            var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await new ZLinkProviderLocationRepository(store).ReadAuthorityAsync(
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId)
                )
            );
            Assert.True(
                ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                    authority.Snapshot.Payload.Span,
                    out var payload
                )
            );
            Assert.Equal(generation, payload.CurrentSpotGeneration);
            Assert.Equal(activation.SpotId, payload.CurrentSpotId);
            Assert.Equal(ZLinkSpotKind.User, payload.CurrentSpotKind);
        }
    }
}
