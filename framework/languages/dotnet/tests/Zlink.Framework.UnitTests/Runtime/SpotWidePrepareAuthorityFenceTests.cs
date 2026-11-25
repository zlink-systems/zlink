using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Theory]
    [InlineData("none")]
    [InlineData("member-version")]
    [InlineData("owner")]
    [InlineData("lease")]
    [InlineData("coordinator-version")]
    public async Task SpotWide_prepare_requires_the_representative_fence_and_sealed_member_version(
        string changed
    )
    {
        await WithSpotWidePrepareAsync(
            new ZLinkInMemoryProviderLocationStore(),
            async (store, spotKey, memberKey, _, originalMember, _) =>
            {
                if (changed == "coordinator-version")
                {
                    var representative = await ReadSpotWideAuthorityAsync(store, spotKey);
                    var relocation = representative.Relocation;
                    await RewriteSpotWideAuthorityAsync(
                        store,
                        spotKey,
                        representative,
                        relocation is null
                            ? null
                            : relocation with
                            {
                                Coordinator = relocation.Coordinator with
                                {
                                    ExpectedStoreVersion = "different-version",
                                },
                            }
                    );
                }
                if (changed == "member-version")
                    await RewriteSpotWideAuthorityAsync(store, memberKey, originalMember);
            },
            prepare =>
                prepare with
                {
                    Coordinator = changed switch
                    {
                        "owner" => prepare.Coordinator with { OwnerId = "different-owner" },
                        "lease" => prepare.Coordinator with
                        {
                            LeaseGeneration = prepare.Coordinator.LeaseGeneration + 1,
                        },
                        _ => prepare.Coordinator,
                    },
                },
            expectReady: changed == "none"
        );
    }

    private static async Task RewriteSpotWideAuthorityAsync(
        IZLinkLocationRepository store,
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot original,
        ZLinkAuthorityRelocation? relocation = null
    )
    {
        var rewrite = Assert
            .IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
                await store.CompareExchangeAuthorityAsync(
                    key,
                    original.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        original.Payload,
                        ZLinkAuthorityGenerationTransition.Preserve,
                        null,
                        null
                    )
                    {
                        Relocation = relocation,
                    }
                )
            )
            .Snapshot;
        Assert.NotEqual(original.StoreVersion, rewrite.StoreVersion);
        Assert.Equal(original.Payload.ToArray(), rewrite.Payload.ToArray());
        Assert.Equal(original.OwnerId, rewrite.OwnerId);
        Assert.Equal(original.OwnerLeaseGeneration, rewrite.OwnerLeaseGeneration);
        Assert.Equal(original.AuthorityOwnerGeneration, rewrite.AuthorityOwnerGeneration);
    }

    private static async Task WithSpotWidePrepareAsync(
        ZLinkInMemoryProviderLocationStore provider,
        Func<
            IZLinkLocationRepository,
            ZLinkAuthorityKey,
            ZLinkAuthorityKey,
            ZLinkAuthoritySnapshot,
            ZLinkAuthoritySnapshot,
            CanonicalRelocationTransportProbe,
            Task
        > atPrepare,
        Func<
            ZLinkServiceWireCodec.RelocationPrepareRecord,
            ZLinkServiceWireCodec.RelocationPrepareRecord
        >? rewritePrepare = null,
        bool expectReady = true
    )
    {
        using var listener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseJoinHandler.TrySetResult();
        trace.ReleaseSourceLeave.TrySetResult();
        trace.ReleaseTargetLifecycle.TrySetResult();
        var probe = new CanonicalRelocationTransportProbe(trace)
        {
            PrepareOverride = rewritePrepare,
        };
        probe.ReleaseCutoverSend.TrySetResult();
        var store = new ZLinkProviderLocationRepository(provider);
        var roots = new SynchronizedRelocationStore();
        var actorId = $"fence-member-{Guid.NewGuid():N}";
        var spotId = $"fence-spot-{Guid.NewGuid():N}";
        var timeout = TimeSpan.FromSeconds(10);
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            provider,
            roots,
            true,
            canonicalTransportProbe: probe,
            relocateSpots: true
        );
        await CreateAndJoinSpotWideMemberAsync(source, spotId, actorId, timeout);
        await trace.WaitAsync("publicJoinCompleted");
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            provider,
            roots,
            true,
            relocateSpots: true
        );
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
        var spotKey = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
        var memberKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var originalSpot = await ReadSpotWideAuthorityAsync(store, spotKey);
        var originalMember = await ReadSpotWideAuthorityAsync(store, memberKey);
        var relocation = source
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = timeout,
                }
            )
            .AsTask();
        try
        {
            await probe.PrepareCallStarted.Task.WaitAsync(timeout);
            await atPrepare(store, spotKey, memberKey, originalSpot, originalMember, probe);
        }
        finally
        {
            probe.ReleasePrepareCall.TrySetResult();
            await relocation;
        }
        if (expectReady)
        {
            Assert.True(probe.ReadyReplyReceived.Task.IsCompletedSuccessfully);
            Assert.Equal(ZLinkFrameworkRelocationOutcome.Relocated, (await relocation).Outcome);
            Assert.Equal(
                target.LocalNodeRid,
                (await ReadSpotWideAuthorityAsync(store, memberKey)).Allocation.Descriptor.Rid
            );
        }
        else
        {
            Assert.False(probe.ReadyReplyReceived.Task.IsCompleted);
            Assert.True(probe.PrepareFailure.Task.IsCompletedSuccessfully);
            Assert.Equal(ZLinkFrameworkRelocationOutcome.Blocked, (await relocation).Outcome);
            var spot = await ReadSpotWideAuthorityAsync(store, spotKey);
            var member = await ReadSpotWideAuthorityAsync(store, memberKey);
            Assert.Equal(originalSpot.OwnerId, spot.OwnerId);
            Assert.Equal(originalSpot.AuthorityOwnerGeneration, spot.AuthorityOwnerGeneration);
            Assert.Null(spot.Relocation);
            Assert.Equal(originalMember.OwnerId, member.OwnerId);
            Assert.Equal(originalMember.AuthorityOwnerGeneration, member.AuthorityOwnerGeneration);
        }
    }

    private static async Task<ZLinkAuthoritySnapshot> ReadSpotWideAuthorityAsync(
        IZLinkLocationRepository store,
        ZLinkAuthorityKey key
    ) =>
        Assert.IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key)).Snapshot;
}
