using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Fact]
    public async Task SpotWide_relocated_member_can_leave_to_target_entry_spot()
    {
        using var listener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var trace = new RelocationBehaviorTrace
        {
            ConfigureTarget = context =>
                context.Handlers.AddActorPacket<LeaveRelocatedSpotHandler, BehaviorActor>(
                    nameof(LeaveRelocatedSpot)
                ),
        };
        trace.ReleaseJoinHandler.TrySetResult();
        trace.ReleaseSourceLeave.TrySetResult();
        trace.ReleaseTargetLifecycle.TrySetResult();

        var store = new ZLinkInMemoryProviderLocationStore();
        var roots = new SynchronizedRelocationStore();
        var actorId = $"relocating-member-{Guid.NewGuid():N}";
        var spotId = $"relocating-spot-{Guid.NewGuid():N}";
        var timeout = TimeSpan.FromSeconds(10);
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            true,
            relocateSpots: true,
            configureServices: services => services.AddTransient<LeaveRelocatedSpotHandler>()
        );
        await CreateAndJoinSpotWideMemberAsync(source, spotId, actorId, timeout);
        await trace.WaitAsync("publicJoinCompleted");

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            true,
            relocateSpots: true,
            configureServices: services => services.AddTransient<LeaveRelocatedSpotHandler>()
        );
        var normalization = SpotWideNormalizationGate.Install(
            target.Runtime.Registration.Locations
        );
        normalization.Release.TrySetResult();
        await WaitUntilAsync(() =>
            source
                .Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                .Node.Status()
                .ActivePeerCount == 1
            && target
                .Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                .Node.Status()
                .ActivePeerCount == 1
        );

        var relocated = await source
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = TimeSpan.FromSeconds(30),
                }
            );
        Assert.Equal(ZLinkFrameworkRelocationOutcome.Relocated, relocated.Outcome);
        var targetClient = target.Services.GetRequiredService<IZLinkActorClient>();
        var beforeLeave = await targetClient
            .RequestToActor(actorId, new BehaviorWork("saved"))
            .Timeout(timeout)
            .Async<BehaviorAck>();
        Assert.Equal("saved", beforeLeave.Marker);
        var left = await targetClient
            .RequestToActor(actorId, new LeaveRelocatedSpot())
            .Timeout(timeout)
            .Async<BehaviorAck>();
        Assert.Equal("target", left.Marker);

        var stored = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await new ZLinkProviderLocationRepository(store).ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId)
            )
        );
        Assert.True(
            ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                stored.Snapshot.Payload.Span,
                out var authority
            )
        );
        Assert.Equal(target.LocalNodeRid, authority.NodeRid);
        Assert.Equal(ZLinkSpotKind.Entry, authority.CurrentSpotKind);
        var manager = target.Services.GetRequiredService<IZLinkActorManager>();
        var membership = Assert.IsType<SpotRef>(await manager.FindSpotAsync(actorId));
        Assert.Equal(membership.SpotId, authority.CurrentSpotId);
        Assert.Equal(membership.ObjectGeneration, authority.CurrentSpotGeneration);

        var afterLeave = await targetClient
            .RequestToActor(actorId, new BehaviorWork("temporary"))
            .Timeout(timeout)
            .Async<BehaviorAck>();
        Assert.Equal("temporary", afterLeave.Marker);
        var spots = target.Services.GetRequiredService<IZLinkSpotManager>();
        var relocatedSpot = Assert.IsType<SpotRef>(await spots.FindAsync(spotId));
        Assert.Equal(target.LocalNodeRid, relocatedSpot.NodeRid);
        // Close starts after the relocation has settled.
        await normalization.Settled.Task.WaitAsync(timeout);
        Assert.True(await spots.CloseAsync(relocatedSpot));
        Assert.Null(await spots.FindAsync(spotId));
    }

    private static async Task CreateAndJoinSpotWideMemberAsync(
        RelocationBehaviorHost source,
        string spotId,
        string actorId,
        TimeSpan timeout
    )
    {
        _ = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(spotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Timeout(timeout)
            .Async();
        _ = await source
            .Services.GetRequiredService<IZLinkActorManager>()
            .Create(actorId, RelocationBehaviorHost.ActorType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(new BehaviorCreate(7))
            .Timeout(timeout)
            .Async();
        _ = await source
            .Services.GetRequiredService<IZLinkActorClient>()
            .RequestToActor(actorId, new BeginBehaviorJoin(spotId))
            .Timeout(timeout)
            .Async<BehaviorAck>();
    }

    private sealed record LeaveRelocatedSpot;

    private sealed class LeaveRelocatedSpotHandler(BehaviorNode node)
        : IZLinkSpotActorRequestHandler<
            BehaviorTargetSpot,
            BehaviorActor,
            LeaveRelocatedSpot,
            BehaviorAck
        >
    {
        public async ValueTask<BehaviorAck> HandleAsync(
            BehaviorTargetSpot spot,
            BehaviorActor actor,
            IZLinkMessageContext context,
            LeaveRelocatedSpot request,
            CancellationToken cancellationToken
        )
        {
            await spot.Context.LeaveActorAsync(actor, cancellationToken);
            return new BehaviorAck(node.Name);
        }
    }
}
