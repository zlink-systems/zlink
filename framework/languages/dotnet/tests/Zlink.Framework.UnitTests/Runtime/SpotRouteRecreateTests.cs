using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Xunit;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SpotRouteRecreateTests(Xunit.Abstractions.ITestOutputHelper output)
{
    [Fact]
    public async Task Cached_ID_request_reaches_recreated_Actor_on_the_same_owner()
    {
        await using var host = await SpotCloseHost.StartAsync();
        await using var caller = await SpotCloseHost.StartAsync(host.Store, clientOnly: true);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        await caller.WaitForPeerAsync(host, cancellation.Token);
        var manager = host.Runtime.Services.GetRequiredService<IZLinkActorManager>();
        var client = caller.Runtime.Services.GetRequiredService<IZLinkActorClient>();
        var actorId = await host.CreateActorAsync();
        var previous = await manager.FindAsync(actorId);
        Assert.NotNull(previous);
        var oldRoute = await host
            .Runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>()
            .ResolveActorRowAsync(new ZLinkActorLocationKey(actorId));
        Assert.NotNull(oldRoute);
        var first = await client
            .RequestToActor(actorId, new SpotCloseActorGeneration())
            .Async<SpotCloseProbeReply>();
        Assert.Equal(previous.Value.ObjectGeneration.ToString(), first.Marker);
        var routes = caller.Runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>();
        var cached = await routes.ResolveActorRowAsync(new ZLinkActorLocationKey(actorId));
        Assert.True(await manager.DestroyAsync(previous.Value));
        var current = Assert
            .IsType<ZLinkActorCreateResult.Created>(await host.GetExistingActorAsync(actorId))
            .Actor;
        Assert.NotEqual(previous.Value.ObjectGeneration, current.ObjectGeneration);
        Assert.Equal(previous.Value.NodeRid, current.NodeRid);
        Assert.Same(cached, await routes.ResolveActorRowAsync(new ZLinkActorLocationKey(actorId)));
        var second = await client
            .RequestToActor(actorId, new SpotCloseActorGeneration())
            .Async<SpotCloseProbeReply>();
        Assert.Equal(current.ObjectGeneration.ToString(), second.Marker);
        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await new ZLinkActorClient(caller.Runtime)
                .RequestToActorExact(
                    SpotCloseHost.MeshName,
                    current,
                    oldRoute.OwnerNodeGeneration,
                    oldRoute.AuthorityOwnerGeneration,
                    checked((ulong)oldRoute.LeaseGeneration),
                    new SpotCloseActorGeneration()
                )
                .Async<SpotCloseProbeReply>()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
    }

    [Fact]
    public async Task Authority_read_yields_until_route_invalidation_completes()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spot = await host.CreateSpotAsync();
        var rows = host.Runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>();
        var repository = host.Runtime.Registration.Locations.ResolveStore()!;
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spot.SpotId);
        var spotKey = new ZLinkSpotLocationKey(spot.SpotId);
        var cached = await rows.ResolveSpotRowAsync(spotKey);
        var external = new ZLinkProviderLocationRepository(host.Store);
        var before = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await external.ReadAuthorityAsync(key))
            .Snapshot;
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await external.CompareExchangeAuthorityAsync(
                key,
                before.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    before.Payload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null
                )
            )
        );
        await AssertYieldsWhileLaneOccupiedAsync(rows, () => repository.ReadAuthorityAsync(key));
        Assert.NotSame(cached, await rows.ResolveSpotRowAsync(spotKey));
    }

    [Fact]
    public async Task DI_connects_authority_observation_for_another_repository_implementation()
    {
        IZLinkLocationRepository repository = new ZLinkInMemoryLocationStore();
        var services = new ServiceCollection();
        services.AddZLinkFramework(builder => builder.UseTestLocationStore());
        services.AddSingleton(repository);
        await using var provider = services.BuildServiceProvider();
        var rows = provider.GetRequiredService<ZLinkStoreLocationResolvers>();
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("other-repository");
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await AssertYieldsWhileLaneOccupiedAsync(rows, () => repository.ReadAuthorityAsync(key))
        );
    }

    [Fact]
    public async Task Wrong_local_node_generation_remains_rejected()
    {
        await using var target = await SpotCloseHost.StartAsync();
        var spot = await target.CreateSpotAsync();
        var current = await target
            .Runtime.Services.GetRequiredService<ZLinkLocationAddressResolvers>()
            .ResolveSpotHandleAsync(spot.SpotId);
        Assert.NotNull(current);
        var snapshot = current.Snapshot with
        {
            NodeGeneration = current.Snapshot.NodeGeneration ^ 1UL,
        };
        var wrong = new ZLinkResolvedSpotHandle(
            snapshot,
            snapshot.Generation,
            _ =>
                throw new InvalidOperationException("A rejected request must not refresh or retry.")
        );
        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await new ZLinkRouteSpotRequestCall<SpotCloseProbeRequest>(
                target.Runtime,
                wrong,
                new SpotCloseProbeRequest("wrong-node-generation")
            )
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<SpotCloseProbeReply>()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, failure.Kind);
        Assert.Equal(0, target.State.HandlerCalls);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Remote_admission_rejects_stale_node_or_inactive_Spot(bool inactive)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = new ZLinkManagedMeshNode(context, "admission");
        await using var target = new ZLinkManagedMeshNode(context, "admission");
        var suffix = Guid.NewGuid().ToString("N");
        source.SetRoutingId(RoutingId.From($"source-{suffix}"));
        target.SetRoutingId(RoutingId.From($"target-{suffix}"));
        source.SetBind($"inproc://source-{suffix}");
        target.SetBind($"inproc://target-{suffix}");
        var spot = (ZLinkManagedSpot)target.GetOrCreateSpot("admission-spot", out var created);
        Assert.True(created);
        source.Start();
        target.Start();
        source.ConnectPeer(target.Status().LocalEndpoint, target.RoutingId);
        await WaitForAdmissionAsync().WaitAsync(TimeSpan.FromSeconds(5));
        if (inactive)
        {
            // Retain the disposed slot while an Actor still belongs to it.
            spot.AddActor();
            spot.Dispose();
        }
        try
        {
            using var head = Message.From(
                ZLinkServiceWireCodec.EncodeSpot(
                    ServiceWireConstants.Command.SpotRequest,
                    1,
                    new MeshOperationId(1, 1),
                    "source-spot",
                    spot.SpotId,
                    spot.LifecycleGeneration,
                    target.RoutingId,
                    inactive
                        ? target.Status().LifecycleGeneration
                        : target.Status().LifecycleGeneration ^ 1UL,
                    spot.AuthorityOwnerGeneration,
                    1,
                    hasMetadata: false
                )
            );
            using var probe = Message.From("probe");
            using var payload = Message.From(
                ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart([probe])
            );
            var reply = await source.RequestDirectWireAsync(
                target.RoutingId,
                [head, payload],
                TimeSpan.FromSeconds(5),
                CancellationToken.None
            );
            try
            {
                Assert.True(
                    ZLinkServiceWireCodec.TryDecodeReply(
                        Assert.Single(reply).AsReadOnlySpan(),
                        out var decoded,
                        out var error
                    )
                );
                Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
                Assert.Equal((int)RequestResult.Conflict, decoded.TerminalResult);
                Assert.Equal(
                    inactive
                        ? ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale
                        : ServiceWireConstants.FrameworkErrorCode.ActorLocationStale,
                    (ServiceWireConstants.FrameworkErrorCode)decoded.FailureCode
                );
            }
            finally
            {
                foreach (var part in reply)
                    part.Dispose();
            }
        }
        finally
        {
            if (inactive)
            {
                spot.RemoveActor();
                target.ReleaseSpot(spot);
            }
        }
        async Task WaitForAdmissionAsync()
        {
            while (source.Status().AdmittedPeerCount != 1 || target.Status().AdmittedPeerCount != 1)
                await Task.Yield();
        }
    }

    private static async Task<T> AssertYieldsWhileLaneOccupiedAsync<T>(
        ZLinkStoreLocationResolvers rows,
        Func<ValueTask<T>> operation
    )
    {
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var returned = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var holding = rows
            .Lane.RunAsync(async () =>
            {
                entered.SetResult();
                await release.Task;
            })
            .AsTask();
        await entered.Task;
        var observing = Task.Run(async () =>
        {
            var pending = operation();
            returned.SetResult();
            return await pending;
        });
        try
        {
            await returned.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(observing.IsCompleted);
        }
        finally
        {
            release.TrySetResult();
            await holding;
            await observing;
        }
        return await observing;
    }

    [Fact]
    public async Task Message_Follow_keeps_its_previous_incarnation_authority_fence()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var first = await host.CreateSpotAsync();
        var handle = await host
            .Runtime.Services.GetRequiredService<ZLinkLocationAddressResolvers>()
            .ResolveSpotHandleAsync(first.SpotId);
        Assert.NotNull(handle);
        var old = handle.Snapshot;
        Assert.True(await host.Manager.CloseAsync(first));
        await host
            .Manager.GetOrCreate(first.SpotId, SpotCloseHost.SpotType)
            .InMesh(SpotCloseHost.MeshName)
            .Async();
        var entry = host.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName).Node.EntrySpot();
        Assert
            .IsAssignableFrom<IZLinkBackendAuthorityObserver>(entry)
            .ObserveSpotAuthority(
                old.NodeRid,
                old.SpotId,
                old.Generation,
                old.NodeGeneration,
                old.AuthorityOwnerGeneration,
                old.OwnerLeaseGeneration
            );
        await using var transport = new ZLinkSpotOutboundTransport(entry, CancellationToken.None);
        var result = await transport.SendMessageFollowToSpotAsync(
            old.NodeRid,
            old.SpotId,
            old.Generation,
            new MeshOperationId(1, 2),
            old.NodeGeneration,
            old.AuthorityOwnerGeneration,
            old.OwnerLeaseGeneration,
            1,
            [Message.From("old-relocation")],
            CancellationToken.None
        );
        Assert.Equal(ZLinkOneWaySubmitStatus.TargetNotFound, result.Status);
        Assert.Equal(0, host.State.HandlerCalls);
    }

    [Fact]
    public async Task Failed_authority_operations_preserve_the_cached_route()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spot = await host.CreateSpotAsync();
        var rows = host.Runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>();
        var repository = host.Runtime.Registration.Locations.ResolveStore()!;
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spot.SpotId);
        var spotKey = new ZLinkSpotLocationKey(spot.SpotId);
        var cached = await rows.ResolveSpotRowAsync(spotKey);
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Conflict>(
            await repository.CompareExchangeAuthorityAsync(
                key,
                "unmatched-opaque-version",
                new ZLinkAuthorityMutation.Delete()
            )
        );
        Assert.Same(cached, await rows.ResolveSpotRowAsync(spotKey));
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await repository.ReadAuthorityAsync(key, cancellation.Token)
        );
        Assert.Same(cached, await rows.ResolveSpotRowAsync(spotKey));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Route_resolved_before_recreation_is_accepted_by_current_target(bool remote)
    {
        using var flow = Trace(
            nameof(Route_resolved_before_recreation_is_accepted_by_current_target)
        );
        await using var target = await SpotCloseHost.StartAsync();
        await using var source = remote
            ? await SpotCloseHost.StartAsync(target.Store, registerUserSpotFactory: false)
            : null;
        var caller = source ?? target;
        if (source is not null)
        {
            using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            await source.WaitForPeerAsync(target, cancellation.Token);
        }
        var id = $"resolved-recreate-{Guid.NewGuid():N}";
        var first = await target
            .Manager.GetOrCreate(id, SpotCloseHost.SpotType)
            .InMesh(SpotCloseHost.MeshName)
            .Async();
        var handle = await caller
            .Runtime.Services.GetRequiredService<ZLinkLocationAddressResolvers>()
            .ResolveSpotHandleAsync(id);
        Assert.NotNull(handle);
        var snapshot = handle.Snapshot;
        Assert.True(await target.Manager.CloseAsync(first.Spot));
        var second = await target
            .Manager.GetOrCreate(id, SpotCloseHost.SpotType)
            .InMesh(SpotCloseHost.MeshName)
            .Async();
        Assert.NotEqual(snapshot.Generation, second.Spot.ObjectGeneration);
        Assert.Equal(snapshot, handle.Snapshot);

        await new ZLinkRouteSpotSendCall<SpotCloseProbeRequest>(
            caller.Runtime,
            handle,
            new SpotCloseProbeRequest("retained-send")
        ).Async();
        var reply = await new ZLinkRouteSpotRequestCall<SpotCloseProbeRequest>(
            caller.Runtime,
            handle,
            new SpotCloseProbeRequest("retained-request")
        )
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<SpotCloseProbeReply>();
        Assert.Equal("retained-request", reply.Marker);
        Assert.Equal(snapshot, handle.Snapshot);
        Assert.True(target.State.HandlerCalls > 0);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Wrong_owner_of_the_same_incarnation_remains_rejected(bool remote)
    {
        await using var target = await SpotCloseHost.StartAsync();
        await using var source = remote
            ? await SpotCloseHost.StartAsync(target.Store, registerUserSpotFactory: false)
            : null;
        var caller = source ?? target;
        if (source is not null)
        {
            using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            await source.WaitForPeerAsync(target, cancellation.Token);
        }
        var spot = await target.CreateSpotAsync();
        var current = await caller
            .Runtime.Services.GetRequiredService<ZLinkLocationAddressResolvers>()
            .ResolveSpotHandleAsync(spot.SpotId);
        Assert.NotNull(current);
        var snapshot = current.Snapshot with
        {
            AuthorityOwnerGeneration = current.Snapshot.AuthorityOwnerGeneration + 1,
        };
        var wrong = new ZLinkResolvedSpotHandle(
            snapshot,
            snapshot.Generation,
            _ =>
                throw new InvalidOperationException("A rejected request must not refresh or retry.")
        );
        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await new ZLinkRouteSpotRequestCall<SpotCloseProbeRequest>(
                caller.Runtime,
                wrong,
                new SpotCloseProbeRequest("wrong-owner")
            )
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<SpotCloseProbeReply>()
        );
        Assert.Equal(
            remote ? ZLinkFrameworkErrorKind.InvalidOperation : ZLinkFrameworkErrorKind.NotFound,
            failure.Kind
        );
        if (remote)
            Assert.Equal(
                (int)
                    Systems
                        .Zlink
                        .Framework
                        .Runtime
                        .Protocol
                        .ServiceWireConstants
                        .FrameworkErrorCode
                        .SpotGenerationStale,
                failure.FrameworkFailureCode
            );
        Assert.Equal(0, target.State.HandlerCalls);
    }

    [Fact]
    public async Task Previous_SpotRef_Close_cannot_close_the_new_incarnation()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var first = await host.CreateSpotAsync();
        Assert.True(await host.Manager.CloseAsync(first));
        var second = await host
            .Manager.GetOrCreate(first.SpotId, SpotCloseHost.SpotType)
            .InMesh(SpotCloseHost.MeshName)
            .Async();
        Assert.NotEqual(first.ObjectGeneration, second.Spot.ObjectGeneration);
        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await host.Manager.CloseAsync(first)
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, failure.Kind);
        Assert.Equal(
            (int)
                Systems
                    .Zlink
                    .Framework
                    .Runtime
                    .Protocol
                    .ServiceWireConstants
                    .FrameworkErrorCode
                    .SpotGenerationStale,
            failure.FrameworkFailureCode
        );
        Assert.Equal("handlerReply", await host.RequestAsync(first.SpotId));
    }

    [Fact]
    public async Task Application_NotFound_preserves_the_cached_route_and_terminal()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spot = await host.CreateSpotAsync();
        var rows = host.Runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>();
        var key = new ZLinkSpotLocationKey(spot.SpotId);
        var cached = await rows.ResolveSpotRowAsync(key);
        var handle = await host
            .Runtime.Services.GetRequiredService<ZLinkLocationAddressResolvers>()
            .ResolveSpotHandleAsync(spot.SpotId);
        Assert.NotNull(handle);
        var terminal = new ZLinkFrameworkException(ZLinkFrameworkErrorKind.NotFound, "application")
        {
            Origin = ZLinkErrorOrigin.Application,
        };
        var calls = 0;
        var observed = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            ZLinkSpotHandleRequestExecution
                .ExecuteAsync<bool>(
                    handle,
                    _ =>
                    {
                        calls++;
                        return ValueTask.FromException<bool>(terminal);
                    },
                    CancellationToken.None
                )
                .AsTask()
        );
        Assert.Same(terminal, observed);
        Assert.Equal(1, calls);
        Assert.Same(cached, await rows.ResolveSpotRowAsync(key));
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task Authority_observation_invalidates_only_a_different_route_version(
        bool throughRead,
        bool delete
    )
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spot = await host.CreateSpotAsync();
        var rows = host.Runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>();
        var repository = host.Runtime.Registration.Locations.ResolveStore()!;
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spot.SpotId);
        var spotKey = new ZLinkSpotLocationKey(spot.SpotId);
        var cached = await rows.ResolveSpotRowAsync(spotKey);
        var before = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await repository.ReadAuthorityAsync(key))
            .Snapshot;
        Assert.Same(cached, await rows.ResolveSpotRowAsync(spotKey));
        // A second repository represents an observation in another runtime.
        var writer = throughRead ? new ZLinkProviderLocationRepository(host.Store) : repository;
        ZLinkAuthorityMutation mutation = delete
            ? new ZLinkAuthorityMutation.Delete()
            : new ZLinkAuthorityMutation.Put(
                before.Payload,
                ZLinkAuthorityGenerationTransition.Preserve,
                null,
                null
            );
        var changed = throughRead
            ? await writer.CompareExchangeAuthorityAsync(key, before.StoreVersion, mutation)
            : await AssertYieldsWhileLaneOccupiedAsync(
                rows,
                () => writer.CompareExchangeAuthorityAsync(key, before.StoreVersion, mutation)
            );
        if (delete)
            Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(changed);
        else
            Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(changed);
        if (throughRead)
        {
            Assert.Same(cached, await rows.ResolveSpotRowAsync(spotKey));
            await AssertYieldsWhileLaneOccupiedAsync(
                rows,
                () => repository.ReadAuthorityAsync(key)
            );
        }
        var after = await rows.ResolveSpotRowAsync(spotKey);
        if (delete)
            Assert.Null(after);
        else
        {
            Assert.NotNull(after);
            Assert.NotSame(cached, after);
            Assert.Same(after, await rows.ResolveSpotRowAsync(spotKey));
        }
    }

    private TestHostMessageFlowListener Trace(string name)
    {
        var path = Path.Combine(
            Environment.GetEnvironmentVariable("SPOT4506_OUT") ?? Path.GetTempPath(),
            $"{name}-{Guid.NewGuid():N}.flow"
        );
        output.WriteLine($"Message flow file: {path}");
        return new TestHostMessageFlowListener(path);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Successful_Commit_observation_invalidates_a_previous_incarnation(
        bool alreadyCommitted
    )
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spot = await host.CreateSpotAsync();
        var rows = host.Runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>();
        var repository = host.Runtime.Registration.Locations.ResolveStore()!;
        var external = new ZLinkProviderLocationRepository(host.Store);
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spot.SpotId);
        var spotKey = new ZLinkSpotLocationKey(spot.SpotId);
        var cached = await rows.ResolveSpotRowAsync(spotKey);
        var previous = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await external.ReadAuthorityAsync(key))
            .Snapshot;
        Assert.True(
            ZLinkUserSpotAuthorityPayloadCodec.TryDecode(previous.Payload.Span, out var payload)
        );
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(
            await external.CompareExchangeAuthorityAsync(
                key,
                previous.StoreVersion,
                new ZLinkAuthorityMutation.Delete()
            )
        );
        var content = new byte[] { 1 };
        var reservation = Assert
            .IsType<ZLinkObjectReserveResult.Reserved>(
                await external.ReserveAsync(
                    new ZLinkObjectReservationRequest(
                        previous.Allocation.ObjectKind,
                        key,
                        payload.StableType,
                        ZLinkInlineCreationIntentCodec.Encode(content),
                        System.Security.Cryptography.SHA256.HashData(content),
                        content.Length,
                        previous.Allocation.Descriptor,
                        previous.Allocation.DescriptorLifecycleGeneration,
                        new ZLinkLocationOwnerToken(
                            previous.OwnerId,
                            previous.OwnerLeaseGeneration
                        ),
                        ZLinkUserSpotAuthorityPayloadCodec.Encode(
                            payload with
                            {
                                State = ZLinkUserSpotAuthorityState.Creating,
                            }
                        ),
                        previous.Allocation.Capacity
                    )
                )
            )
            .Reservation;
        Assert.Same(cached, await rows.ResolveSpotRowAsync(spotKey));
        if (alreadyCommitted)
            Assert.IsType<ZLinkObjectCommitResult.Committed>(
                await external.CommitAsync(reservation, previous.Payload)
            );
        var committed = await AssertYieldsWhileLaneOccupiedAsync(
            rows,
            () => repository.CommitAsync(reservation, previous.Payload)
        );
        if (alreadyCommitted)
            Assert.IsType<ZLinkObjectCommitResult.AlreadyCommitted>(committed);
        else
            Assert.IsType<ZLinkObjectCommitResult.Committed>(committed);
        var current = await rows.ResolveSpotRowAsync(spotKey);
        Assert.NotNull(current);
        Assert.NotSame(cached, current);
        Assert.Equal(reservation.ObjectGeneration, current.SpotGeneration);
        Assert.Same(current, await rows.ResolveSpotRowAsync(spotKey));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Direct_send_reaches_current_Ready_incarnation_after_Close_and_GetOrCreate(
        bool warmRoute
    )
    {
        var directory = Environment.GetEnvironmentVariable("SPOT4506_OUT") ?? Path.GetTempPath();
        using var flow = new TestHostMessageFlowListener(
            $"{directory}/dotnet-4506-{warmRoute}.flow"
        );
        await using var host = await SpotCloseHost.StartAsync();
        var client = host.Runtime.Services.GetRequiredService<IZLinkSpotClient>();
        var id = $"dspot-4506-{warmRoute}-{Guid.NewGuid():N}";
        var first = await host
            .Manager.GetOrCreate(id, SpotCloseHost.SpotType)
            .InMesh(SpotCloseHost.MeshName)
            .Async();
        Assert.Equal(ZLinkSpotCreateState.Created, first.State);
        await ObserveAuthority("first");
        if (warmRoute)
            Assert.Equal("handlerReply", await host.RequestAsync(id));
        Assert.True(await host.Manager.CloseAsync(first.Spot));
        await ObserveAuthority("closed");
        var second = await host
            .Manager.GetOrCreate(id, SpotCloseHost.SpotType)
            .InMesh(SpotCloseHost.MeshName)
            .Async();
        Assert.Equal(ZLinkSpotCreateState.Created, second.State);
        await ObserveAuthority("second");
        output.WriteLine(
            $"Spot={id} incarnation={first.Spot.ObjectGeneration}->{second.Spot.ObjectGeneration} warmRoute={warmRoute}"
        );
        await client.SendToSpot(id, new SpotCloseProbeRequest("new-incarnation")).Async();
        Assert.Equal("handlerReply", await host.RequestAsync(id));
        async Task ObserveAuthority(string phase)
        {
            var repository = host.Runtime.Registration.Locations.ResolveStore()!;
            var result = await repository.ReadAuthorityAsync(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(id)
            );
            if (result is ZLinkAuthorityReadResult.Found found)
            {
                var s = found.Snapshot;
                output.WriteLine(
                    $"authority record phase={phase} incarnation={s.ObjectGeneration} authorityOwnerGeneration={s.AuthorityOwnerGeneration} StoreVersion={s.StoreVersion} owner={s.OwnerId} ownerLeaseGeneration={s.OwnerLeaseGeneration}"
                );
            }
            else
                output.WriteLine($"authority record phase={phase} Missing");
        }
    }
}
