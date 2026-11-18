using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.UnitTests.Runtime;

namespace Zlink.Framework.UnitTests;

public sealed partial class LocationRuntimeTests
{
    [Theory]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task DiscardedAuthority_Cleans_Before_Another_Host_GetOrCreate(
        bool outage,
        bool expired
    )
    {
        var time = new ControllableTimeProvider();
        var store = new ZLinkProviderLocationRepository(
            new ZLinkInMemoryProviderLocationStore(time),
            time
        );
        var flaky = new OutageLeaseStore(store);
        await using var first = await DiscardedAuthorityHost.Start(flaky, time);
        var locations = first.Locations;
        await first
            .Provider.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate("discarded-spot", "outage-execution")
            .InMesh("discarded-authority")
            .Async();
        await first
            .Provider.GetRequiredService<IZLinkActorManager>()
            .GetOrCreate("discarded-actor", "outage-actor")
            .InMesh("discarded-authority")
            .Async();
        var spot = Assert.Single(first.Runtime.GetSpotNodeRuntime("discarded-authority").Spots);
        Assert.True(first.Runtime.TryGetActorState("discarded-actor", out var actor));
        var spotKey = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("discarded-spot");
        var actorKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey("discarded-actor");
        Assert.True(
            (
                await spot.JoinActorAsync(actor.Actor!, ZLinkMessage.Empty, CancellationToken.None)
            ).Accepted
        );
        Assert.True(spot.ContainsActor("discarded-actor"));
        var oldSpot = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(spotKey))
            .Snapshot;
        var oldActor = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(actorKey))
            .Snapshot;
        time.AdvanceMonotonic(TimeSpan.FromSeconds(1));
        if (outage)
        {
            flaky.Fail = true;
            Assert.False(await locations.RenewOwnerLeaseOnceAsync());
            var episode = locations.GetHealthSnapshot().StoreOutage!;
            var remaining = episode.Lifetime - time.GetElapsedTime(episode.LeaseOperationStartedAt);
            var beforeDiscard = flaky.DiscardedCleanupCalls;
            if (expired)
                time.Advance(remaining);
            else
                time.AdvanceMonotonic(remaining);
            Assert.Equal(beforeDiscard, flaky.DiscardedCleanupCalls);
        }
        else
        {
            time.AdvanceMonotonicWithoutTimers(TimeSpan.FromSeconds(40));
            flaky.Stale = true;
            Assert.False(await locations.RenewOwnerLeaseOnceAsync());
            flaky.Stale = false;
            await store.ReleaseOwnerLeaseAsync(locations.OwnerToken);
        }
        Assert.True(spot.IsDisposed);
        Assert.True(actor.ContextInvalidated);
        await first.Runtime.DiscardLocationExecutionAsync(spot);
        await first.Runtime.DiscardLocationExecutionAsync(actor);
        flaky.Fail = false;
        Assert.True(await locations.RenewOwnerLeaseOnceAsync());
        if (expired)
        {
            Assert.IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(actorKey));
            time.AdvanceMonotonic(TimeSpan.FromSeconds(15) - TimeSpan.FromTicks(1));
            Assert.True(await locations.RenewOwnerLeaseOnceAsync());
            Assert.IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(actorKey));
            time.AdvanceMonotonic(TimeSpan.FromTicks(1));
            Assert.True(await locations.RenewOwnerLeaseOnceAsync());
        }
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(await store.ReadAuthorityAsync(actorKey));
        if (expired)
        {
            Assert.IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(spotKey));
            time.AdvanceMonotonic(TimeSpan.FromSeconds(15) - TimeSpan.FromTicks(1));
            Assert.True(await locations.RenewOwnerLeaseOnceAsync());
            Assert.IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(spotKey));
            time.AdvanceMonotonic(TimeSpan.FromTicks(1));
            Assert.True(await locations.RenewOwnerLeaseOnceAsync());
        }
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(await store.ReadAuthorityAsync(spotKey));
        var capacity = Assert
            .Single(await store.ListAllMeshNodesAsync("discarded-authority"))
            .Capacity;
        Assert.Equal(0, capacity.Actors.Active);
        Assert.Equal(0, capacity.Spots.Active);
        Assert.False(spot.ContainsActor("discarded-actor"));
        Assert.Null(actor.Actor);
        Assert.Empty(first.Runtime.GetSpotNodeRuntime("discarded-authority").Spots);
        await first.Runtime.ForceStopAsync(CancellationToken.None);
        await first.Locations.StopAsync();
        await using var second = await DiscardedAuthorityHost.Start(store, time);
        await second
            .Provider.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate("discarded-spot", "outage-execution")
            .InMesh("discarded-authority")
            .Async();
        await second
            .Provider.GetRequiredService<IZLinkActorManager>()
            .GetOrCreate("discarded-actor", "outage-actor")
            .InMesh("discarded-authority")
            .Async();
        var newSpot = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(spotKey))
            .Snapshot;
        var newActor = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(actorKey))
            .Snapshot;
        Assert.NotEqual(oldSpot.ObjectGeneration, newSpot.ObjectGeneration);
        Assert.NotEqual(oldActor.ObjectGeneration, newActor.ObjectGeneration);
        Assert.Equal(second.Locations.OwnerId, newActor.OwnerId);
        Assert.Equal(1, second.Probe.FactoryCalls);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DiscardedAuthority_Preserves_Another_Incarnation(bool provider)
    {
        var time = new ControllableTimeProvider();
        IZLinkLocationRepository store = provider
            ? new ZLinkProviderLocationRepository(
                new ZLinkInMemoryProviderLocationStore(time),
                time
            )
            : new ZLinkInMemoryLocationStore(time);
        await using var host = await DiscardedAuthorityHost.Start(store, time);
        await host
            .Provider.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate("successor", "outage-execution")
            .InMesh("discarded-authority")
            .Async();
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("successor");
        var old = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key))
            .Snapshot;
        var replacement = Assert
            .IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
                await store.CompareExchangeAuthorityAsync(
                    key,
                    old.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        old.Payload,
                        ZLinkAuthorityGenerationTransition.Reincarnate,
                        null,
                        null
                    )
                )
            )
            .Snapshot;
        Assert.True(
            await store.CleanupDiscardedAuthorityAsync(key, old, host.Locations.OwnerToken)
        );
        var current = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key))
            .Snapshot;
        Assert.Equal(replacement.StoreVersion, current.StoreVersion);
        Assert.NotEqual(old.ObjectGeneration, current.ObjectGeneration);
        await host.Runtime.DiscardLocationExecutionAsync(
            Assert.Single(host.Runtime.GetSpotNodeRuntime("discarded-authority").Spots)
        );
        Assert.True(await host.Locations.RenewOwnerLeaseOnceAsync());
        Assert.Equal(
            replacement.StoreVersion,
            Assert
                .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key))
                .Snapshot.StoreVersion
        );
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DiscardedAuthority_Spot_Keeps_Remote_Member(bool provider)
    {
        var time = new ControllableTimeProvider();
        IZLinkLocationRepository store = provider
            ? new ZLinkProviderLocationRepository(
                new ZLinkInMemoryProviderLocationStore(time),
                time
            )
            : new ZLinkInMemoryLocationStore(time);
        await using var remote = await DiscardedAuthorityHost.Start(store, time, spots: false);
        await remote
            .Provider.GetRequiredService<IZLinkActorManager>()
            .GetOrCreate("remote-member", "outage-actor")
            .InMesh("discarded-authority")
            .Async();
        await using var first = await DiscardedAuthorityHost.Start(store, time);
        await first
            .Provider.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate("remote-member-spot", "outage-execution")
            .InMesh("discarded-authority")
            .Async();
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("remote-member-spot");
        var spot = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key))
            .Snapshot;
        Assert.Equal(first.Locations.OwnerId, spot.OwnerId);
        var actorKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey("remote-member");
        var actor = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(actorKey))
            .Snapshot;
        Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecode(actor.Payload.Span, out var payload));
        var memberPayload = ZLinkActorAuthorityPayloadCodec.Encode(
            payload with
            {
                CurrentSpotKind = ZLinkSpotKind.User,
                CurrentSpotId = "remote-member-spot",
                CurrentSpotGeneration = spot.ObjectGeneration,
            }
        );
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await store.CompareExchangeAuthorityAsync(
                actorKey,
                actor.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    memberPayload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null
                )
            )
        );
        Assert.False(
            await store.CleanupDiscardedAuthorityAsync(key, spot, first.Locations.OwnerToken)
        );
        Assert.IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key));
        var member = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(actorKey))
            .Snapshot;
        Assert.Equal(remote.Locations.OwnerId, member.OwnerId);
        Assert.True(
            await store.CleanupDiscardedAuthorityAsync(
                actorKey,
                member,
                remote.Locations.OwnerToken
            )
        );
        Assert.True(
            await store.CleanupDiscardedAuthorityAsync(key, spot, first.Locations.OwnerToken)
        );
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(await store.ReadAuthorityAsync(key));
    }

    [Fact]
    public async Task DiscardedAuthority_Spot_ScanFailure_And_Expired_Snapshot_Do_Not_Mean_Empty()
    {
        var time = new ControllableTimeProvider();
        var raw = new DiscardedAuthorityProvider(new ZLinkInMemoryProviderLocationStore(time));
        var store = new ZLinkProviderLocationRepository(raw, time);
        await using var host = await DiscardedAuthorityHost.Start(store, time);
        await host
            .Provider.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate("scan-spot", "outage-execution")
            .InMesh("discarded-authority")
            .Async();
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("scan-spot");
        var spot = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key))
            .Snapshot;
        raw.FailScan = true;
        await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            store.CleanupDiscardedAuthorityAsync(key, spot, host.Locations.OwnerToken).AsTask()
        );
        Assert.IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key));
        raw.FailScan = false;
        raw.ExpireScanOnce = true;
        var scans = raw.Scans;
        Assert.True(
            await store.CleanupDiscardedAuthorityAsync(key, spot, host.Locations.OwnerToken)
        );
        Assert.True(raw.Scans >= scans + 2);
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(await store.ReadAuthorityAsync(key));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DiscardedAuthority_Waits_For_Relocation_Settlement(bool provider)
    {
        var time = new ControllableTimeProvider();
        IZLinkLocationRepository store = provider
            ? new ZLinkProviderLocationRepository(
                new ZLinkInMemoryProviderLocationStore(time),
                time
            )
            : new ZLinkInMemoryLocationStore(time);
        await using var host = await DiscardedAuthorityHost.Start(store, time);
        await host
            .Provider.GetRequiredService<IZLinkActorManager>()
            .GetOrCreate("relocating-actor", "outage-actor")
            .InMesh("discarded-authority")
            .Async();
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey("relocating-actor");
        var old = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key))
            .Snapshot;
        Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecode(old.Payload.Span, out var payload));
        var preparing = await new ZLinkStandaloneActorRelocationPrecommitCoordinator(
            store
        ).BeginPreparingAsync(old, payload, Guid.NewGuid(), 1, CancellationToken.None);
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            store.CleanupDiscardedAuthorityAsync(key, old, host.Locations.OwnerToken).AsTask()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        Assert.Equal(
            preparing.StoreVersion,
            Assert
                .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key))
                .Snapshot.StoreVersion
        );
        Assert.Equal(
            1,
            Assert
                .Single(await store.ListAllMeshNodesAsync("discarded-authority"))
                .Capacity.Actors.Active
        );
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await store.CompareExchangeAuthorityAsync(
                key,
                preparing.StoreVersion,
                new ZLinkAuthorityMutation.Restore(old.Payload, host.Locations.OwnerToken)
            )
        );
        Assert.True(
            await store.CleanupDiscardedAuthorityAsync(key, old, host.Locations.OwnerToken)
        );
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(await store.ReadAuthorityAsync(key));
    }

    [Fact]
    public async Task DiscardedAuthority_Same_Node_Join_Settles_Before_Spot_Cleanup()
    {
        var time = new ControllableTimeProvider();
        var store = new ZLinkProviderLocationRepository(
            new ZLinkInMemoryProviderLocationStore(time),
            time
        );
        var controlled = new OutageLeaseStore(store);
        await using var host = await DiscardedAuthorityHost.Start(controlled, time);
        await host
            .Provider.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate("settling-spot", "outage-execution")
            .InMesh("discarded-authority")
            .Async();
        await host
            .Provider.GetRequiredService<IZLinkActorManager>()
            .GetOrCreate("settling-actor", "outage-actor")
            .InMesh("discarded-authority")
            .Async();
        var spot = Assert.Single(host.Runtime.GetSpotNodeRuntime("discarded-authority").Spots);
        Assert.True(host.Runtime.TryGetActorState("settling-actor", out var actor));
        var committed = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        controlled.AfterMembershipCommit = async () =>
        {
            committed.TrySetResult();
            await release.Task;
        };
        var join = spot.JoinActorAsync(actor.Actor!, ZLinkMessage.Empty, CancellationToken.None)
            .AsTask();
        Task? disposal = null;
        try
        {
            await committed.Task.WaitAsync(TimeSpan.FromSeconds(5));
            disposal = host.Runtime.DiscardLocationExecutionAsync(spot).AsTask();
            Assert.False(disposal.IsCompleted);
            Assert.True(
                await host
                    .Locations.RenewOwnerLeaseOnceAsync()
                    .AsTask()
                    .WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(0, controlled.DiscardedCleanupCalls);
            Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await store.ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("settling-spot")
                )
            );
        }
        finally
        {
            release.TrySetResult();
        }
        try
        {
            await join.WaitAsync(TimeSpan.FromSeconds(5));
        }
        catch (OperationCanceledException) { }
        catch (ZLinkFrameworkException error) when (error.Kind == ZLinkFrameworkErrorKind.NotFound)
        { }
        await disposal!.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Null(actor.Actor);
        Assert.True(
            await host
                .Locations.RenewOwnerLeaseOnceAsync()
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5))
        );
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await store.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey("settling-actor")
            )
        );
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await store.ReadAuthorityAsync(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("settling-spot")
            )
        );
    }

    [Fact]
    public async Task DiscardedAuthority_Same_Node_Join_Lost_Reply_Settles_After_Recovery()
    {
        var time = new ControllableTimeProvider();
        var store = new ZLinkProviderLocationRepository(
            new ZLinkInMemoryProviderLocationStore(time),
            time
        );
        var controlled = new OutageLeaseStore(store);
        await using var host = await DiscardedAuthorityHost.Start(controlled, time);
        await host
            .Provider.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate("settling-spot", "outage-execution")
            .InMesh("discarded-authority")
            .Async();
        await host
            .Provider.GetRequiredService<IZLinkActorManager>()
            .GetOrCreate("settling-actor", "outage-actor")
            .InMesh("discarded-authority")
            .Async();
        var spot = Assert.Single(host.Runtime.GetSpotNodeRuntime("discarded-authority").Spots);
        Assert.True(host.Runtime.TryGetActorState("settling-actor", out var actor));
        var committed = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var reconfirmed = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var unavailable = false;
        controlled.BeforeAuthorityRead = () =>
        {
            if (unavailable)
            {
                reconfirmed.TrySetResult();
                throw new TimeoutException("Membership confirmation Store unavailable.");
            }
            return ValueTask.CompletedTask;
        };
        controlled.AfterMembershipCommit = async () =>
        {
            committed.TrySetResult();
            await release.Task;
            unavailable = true;
            throw new TimeoutException("Applied membership reply lost.");
        };
        var join = spot.JoinActorAsync(actor.Actor!, ZLinkMessage.Empty, CancellationToken.None)
            .AsTask();
        Task? disposal = null;
        try
        {
            await committed.Task.WaitAsync(TimeSpan.FromSeconds(5));
            disposal = host.Runtime.DiscardLocationExecutionAsync(spot).AsTask();
            Assert.False(disposal.IsCompleted);
            Assert.True(
                await host
                    .Locations.RenewOwnerLeaseOnceAsync()
                    .AsTask()
                    .WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(0, controlled.DiscardedCleanupCalls);
            Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await store.ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("settling-spot")
                )
            );
        }
        finally
        {
            release.TrySetResult();
        }
        await reconfirmed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(disposal!.IsCompleted);
        Assert.Equal(0, controlled.DiscardedCleanupCalls);
        Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("settling-spot")
            )
        );
        unavailable = false;
        Assert.True(await host.Locations.RenewOwnerLeaseOnceAsync());
        try
        {
            await join.WaitAsync(TimeSpan.FromSeconds(5));
        }
        catch (OperationCanceledException) { }
        catch (ZLinkFrameworkException error) when (error.Kind == ZLinkFrameworkErrorKind.NotFound)
        { }
        await disposal!.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Null(actor.Actor);
        Assert.True(
            await host
                .Locations.RenewOwnerLeaseOnceAsync()
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5))
        );
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await store.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey("settling-actor")
            )
        );
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await store.ReadAuthorityAsync(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("settling-spot")
            )
        );
    }

    [Fact]
    public async Task DiscardedAuthority_Remote_Join_Settles_Before_Cutover_Arrives()
    {
        var trace = new RelocationBehaviorTrace { ExpectedJoinAccepted = false };
        var transport = new CanonicalRelocationTransportProbe(trace, holdTargetReady: true);
        var provider = new RecordingLocationStore(new ZLinkInMemoryProviderLocationStore(), trace);
        var roots = new SynchronizedRelocationStore();
        var actorId = $"discarded-joining-{Guid.NewGuid():N}";
        var spotId = $"discarded-target-{Guid.NewGuid():N}";
        trace.ActorId = actorId;
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            provider,
            roots,
            registerTargetSpot: false,
            canonicalTransportProbe: transport
        );
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await source
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Async()
        );
        trace.SourceObjectGeneration = created.Actor.ObjectGeneration;
        trace.SourceNodeRid = created.Actor.NodeRid;
        await provider.ObserveActorAuthorityAsync(actorId);
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            provider,
            roots,
            registerTargetSpot: true,
            canonicalTransportProbe: transport
        );
        var createdSpot = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(spotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        Assert.Equal(target.LocalNodeRid, createdSpot.Spot.NodeRid);
        trace.TargetNodeRid = target.LocalNodeRid;
        try
        {
            var join = source
                .Services.GetRequiredService<IZLinkActorClient>()
                .RequestToActor(actorId, new BeginBehaviorJoin(spotId))
                .Async<BehaviorAck>()
                .AsTask();
            await trace.WaitAsync("relocationRequested");
            trace.ReleaseJoinHandler.TrySetResult();
            await join;
            await trace.WaitAsync("targetAdmissionReplied");
            await transport.PrepareCallStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            transport.ReleasePrepareCall.TrySetResult();
            await transport.TargetPreparedBeforeReadySend.Task.WaitAsync(TimeSpan.FromSeconds(5));
            transport.ReleaseTargetReadySend.TrySetResult();
            await transport.ReadyReplyReceived.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await transport.CutoverSendStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var spot = Assert.Single(
                target.Runtime.GetSpotNodeRuntime(RelocationBehaviorHost.MeshName).Spots
            );
            Assert.Contains(
                target.Runtime.StandaloneActorRelocationRuntime._targetAttempts.Values,
                slot => slot.Stage is not null
            );
            await target
                .Runtime.DiscardLocationExecutionAsync(spot)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5));
            Assert.DoesNotContain(
                target.Runtime.StandaloneActorRelocationRuntime._targetAttempts.Values,
                slot => slot.Stage is not null
            );
            Assert.True(spot.IsDisposed);
            Assert.False(transport.ReleaseCutoverSend.Task.IsCompleted);
            Assert.True(
                await target
                    .Services.GetRequiredService<ZLinkLocationRuntime>()
                    .RenewOwnerLeaseOnceAsync()
            );
            var store = target.Runtime.Registration.Locations.ResolveStore()!;
            Assert.IsType<ZLinkAuthorityReadResult.Missing>(
                await store.ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId)
                )
            );
            var actor = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(
                    await store.ReadAuthorityAsync(
                        ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId)
                    )
                )
                .Snapshot;
            Assert.Equal(
                source.Services.GetRequiredService<ZLinkLocationRuntime>().OwnerId,
                actor.OwnerId
            );
            transport.ReleaseCutoverSend.TrySetResult();
            await transport.CutoverSendSubmitted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.IsType<ZLinkAuthorityReadResult.Missing>(
                await store.ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId)
                )
            );
        }
        finally
        {
            transport.ReleasePrepareCall.TrySetResult();
            transport.ReleaseTargetReadySend.TrySetResult();
            transport.ReleaseCutoverSend.TrySetResult();
            trace.ReleaseTargetLifecycle.TrySetResult();
            trace.ReleaseSourceLeave.TrySetResult();
        }
    }

    private sealed class DiscardedAuthorityProvider(IZLinkLocationStore inner) : IZLinkLocationStore
    {
        internal int Scans;
        internal bool FailScan;
        internal bool ExpireScanOnce;

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken ct = default
        )
        {
            return inner.ReadAsync(key, ct);
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken ct = default
        )
        {
            return inner.WriteAsync(request, ct);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken ct = default
        )
        {
            Interlocked.Increment(ref Scans);
            if (FailScan)
                throw new InvalidOperationException("Injected membership snapshot failure.");
            if (ExpireScanOnce)
            {
                ExpireScanOnce = false;
                return ValueTask.FromResult<ZLinkStoreScanResult>(
                    new ZLinkStoreScanResult.Expired()
                );
            }
            return inner.ScanAsync(request, ct);
        }
    }

    private sealed class DiscardedAuthoritySpot(IZLinkSpotContext context)
        : IZLinkSpot<OutageExecutionActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken ct
        ) => ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

        public ValueTask OnJoinedActorAsync(OutageExecutionActor actor, CancellationToken ct) =>
            ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(OutageExecutionActor actor, CancellationToken ct) =>
            ValueTask.CompletedTask;
    }

    private sealed class DiscardedAuthorityHost(ServiceProvider provider) : IAsyncDisposable
    {
        internal ServiceProvider Provider => provider;
        internal ZLinkFrameworkRuntime Runtime =>
            provider.GetRequiredService<ZLinkFrameworkRuntime>();
        internal ZLinkLocationRuntime Locations =>
            provider.GetRequiredService<ZLinkLocationRuntime>();
        internal OutageExecutionProbe Probe => provider.GetRequiredService<OutageExecutionProbe>();

        internal static async Task<DiscardedAuthorityHost> Start(
            IZLinkLocationRepository store,
            ControllableTimeProvider time,
            bool spots = true
        )
        {
            var services = new ServiceCollection().AddSingleton(new OutageExecutionProbe());
            ZLinkLocationOptions options = null!;
            services.AddZLinkFramework(builder =>
            {
                builder.DisableImplicitHandlerAutoRegistration();
                builder.UseTestLocationStore();
                builder.AddRelocationStore(new InMemoryRelocationStore());
                builder.UseTestTimeProvider(time);
                options = builder.ConfigureLocations();
                options.StoreOutageRetention = true;
                var server = builder
                    .AddRouteMesh("discarded-authority")
                    .Listen("tcp://127.0.0.1:0")
                    .Objects()
                    .Server();
                if (spots)
                    server.AddSpotFactory<DiscardedAuthoritySpot>(
                        "outage-execution",
                        factory => factory.RecreateOnRelocation()
                    );
                server.AddEntrySpot<OutageExecutionEntry>();
                server.AddActorFactory<OutageExecutionActor, OutageExecutionActorFactory>(
                    "outage-actor",
                    factory => factory.DisableRelocation()
                );
            });
            services.AddSingleton<IZLinkLocationRepository>(store);
            services.AddSingleton(sp => new ZLinkLocationRuntime(options, store, time));
            var result = new DiscardedAuthorityHost(services.BuildServiceProvider());
            result.Provider.GetRequiredService<ZLinkFrameworkRegistration>().Locations._repository =
                store;
            await result
                .Provider.GetServices<IHostedService>()
                .Single(service => service is ZLinkFrameworkHostedService)
                .StartAsync(CancellationToken.None);
            return result;
        }

        public async ValueTask DisposeAsync()
        {
            Probe.ReleaseCreating.TrySetResult();
            Probe.ReleaseClosing.TrySetResult();
            await Runtime.ForceStopAsync(CancellationToken.None);
            await Locations.StopAsync();
            await provider.DisposeAsync();
        }
    }
}
