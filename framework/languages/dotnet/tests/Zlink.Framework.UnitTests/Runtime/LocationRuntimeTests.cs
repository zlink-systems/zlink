using System.Diagnostics;
using System.Security.Cryptography;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class LocationRuntimeTests
{
    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task StoreOutageRetention_Latches_At_Original_Cutoff_Without_Turns(
        bool provider,
        bool retention
    )
    {
        var time = new ControllableTimeProvider();
        IZLinkLocationRepository store = provider
            ? new ZLinkProviderLocationRepository(new ZLinkInMemoryProviderLocationStore(time))
            : new ZLinkInMemoryLocationStore(time);
        var flaky = new OutageLeaseStore(store);
        var options = new ZLinkLocationOptions { StoreOutageRetention = retention };
        var admissionLifetime = options.OwnerLeaseTtl - options.OwnerLeaseFencingMargin;
        var outageLifetime = retention
            ? options.OwnerLeaseTtl
                + options.OwnerLeaseFencingMargin
                + options.OwnerLeaseFencingMargin
            : admissionLifetime;
        await using var runtime = new ZLinkLocationRuntime(options, flaky, time);
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.Throws<InvalidOperationException>(() => options.StoreOutageRetention = !retention);
        time.AdvanceMonotonic(TimeSpan.FromSeconds(1));
        flaky.Fail = true;
        Assert.False(await runtime.RenewOwnerLeaseOnceAsync());
        var episode = Assert.IsType<ZLinkStoreOutageEpisode>(
            runtime.GetHealthSnapshot().StoreOutage
        );
        Assert.Equal(TimeSpan.FromSeconds(1).Ticks, episode.ObservedAt);
        Assert.Equal(0, episode.LeaseOperationStartedAt);
        Assert.Equal(outageLifetime, episode.Lifetime);
        // Admission keeps its existing deadline, including when retention is enabled.
        time.AdvanceMonotonic(episode.Lifetime - TimeSpan.FromSeconds(1) - TimeSpan.FromTicks(1));
        Assert.False(runtime.GetHealthSnapshot().StoreOutage!.Discarded);
        time.AdvanceMonotonic(TimeSpan.FromTicks(1));
        Assert.True(runtime.GetHealthSnapshot().StoreOutage!.Discarded);
        Assert.False(runtime.IsOwnerAdmissionOpen);
        flaky.Fail = false;
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.True(runtime.IsOwnerAdmissionOpen);
        Assert.True(runtime.GetHealthSnapshot().StoreOutage!.Discarded);
        Assert.Equal(episode.Lifetime, runtime.GetHealthSnapshot().StoreOutage!.Lifetime);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task StoreOutageRetention_Renewal_Before_T_Closes_Episode(bool provider)
    {
        var time = new ControllableTimeProvider();
        IZLinkLocationRepository store = provider
            ? new ZLinkProviderLocationRepository(new ZLinkInMemoryProviderLocationStore(time))
            : new ZLinkInMemoryLocationStore(time);
        var flaky = new OutageLeaseStore(store);
        var options = new ZLinkLocationOptions { StoreOutageRetention = true };
        await using var runtime = new ZLinkLocationRuntime(options, flaky, time);
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        time.AdvanceMonotonic(TimeSpan.FromSeconds(1));
        flaky.Fail = true;
        Assert.False(await runtime.RenewOwnerLeaseOnceAsync());
        time.AdvanceMonotonic(
            options.OwnerLeaseTtl - options.OwnerLeaseFencingMargin - TimeSpan.FromSeconds(2)
        );
        flaky.Fail = false;
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.Null(runtime.GetHealthSnapshot().StoreOutage);
        time.AdvanceMonotonic(TimeSpan.FromSeconds(1));
        Assert.Null(runtime.GetHealthSnapshot().StoreOutage);
        Assert.Equal(0, time.ActiveTimerCount);
    }

    [Fact]
    public async Task StoreOutageRetention_Default_And_Definitive_Response_Do_Not_Open_Episode()
    {
        Assert.False(new ZLinkLocationOptions().StoreOutageRetention);
        var time = new ControllableTimeProvider();
        var flaky = new OutageLeaseStore(new ZLinkInMemoryLocationStore(time));
        await using var runtime = new ZLinkLocationRuntime(new ZLinkLocationOptions(), flaky, time);
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        flaky.Stale = true;
        Assert.False(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.Null(runtime.GetHealthSnapshot().StoreOutage);
        Assert.Equal(0, time.ActiveTimerCount);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task StoreOutageRetention_Read_Failure_Freezes_T_Across_Late_Claim(bool provider)
    {
        var time = new ControllableTimeProvider();
        IZLinkLocationRepository store = provider
            ? new ZLinkProviderLocationRepository(new ZLinkInMemoryProviderLocationStore(time))
            : new ZLinkInMemoryLocationStore(time);
        var flaky = new OutageLeaseStore(store) { HoldClaim = true };
        var options = new ZLinkLocationOptions { StoreOutageRetention = true };
        await using var runtime = new ZLinkLocationRuntime(options, flaky, time);
        var lateClaim = runtime.RenewOwnerLeaseOnceAsync().AsTask();
        await flaky.ClaimStarted.Task;
        flaky.HoldClaim = false;
        flaky.ConflictClaim = true;
        flaky.HoldRead = true;
        var confirmation = runtime.RenewOwnerLeaseOnceAsync().AsTask();
        await flaky.ReadStarted.Task;
        flaky.ConflictClaim = false;
        flaky.HoldRead = false;
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        time.AdvanceMonotonic(TimeSpan.FromSeconds(1));
        flaky.FailRead = true;
        flaky.ReleaseRead.TrySetResult();
        await Assert.ThrowsAsync<ZLinkOwnerLeaseClaimRejectedException>(() => confirmation);
        var episode = runtime.GetHealthSnapshot().StoreOutage;
        Assert.NotNull(episode);
        var retentionLifetime =
            options.OwnerLeaseTtl
            + options.OwnerLeaseFencingMargin
            + options.OwnerLeaseFencingMargin;
        Assert.Equal(0, episode.LeaseOperationStartedAt);
        Assert.Equal(retentionLifetime, episode.Lifetime);
        flaky.ReleaseClaim.TrySetResult();
        Assert.True(await lateClaim);
        Assert.Equal(episode, runtime.GetHealthSnapshot().StoreOutage);
        time.AdvanceMonotonic(
            retentionLifetime
                - time.GetElapsedTime(episode.LeaseOperationStartedAt)
                - TimeSpan.FromTicks(1)
        );
        Assert.False(runtime.GetHealthSnapshot().StoreOutage!.Discarded);
        time.AdvanceMonotonic(TimeSpan.FromTicks(1));
        Assert.True(runtime.GetHealthSnapshot().StoreOutage!.Discarded);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task StoreOutageRetention_Late_Renew_Cannot_Undo_Original_T(bool provider)
    {
        var time = new ControllableTimeProvider();
        IZLinkLocationRepository store = provider
            ? new ZLinkProviderLocationRepository(new ZLinkInMemoryProviderLocationStore(time))
            : new ZLinkInMemoryLocationStore(time);
        var flaky = new OutageLeaseStore(store);
        var options = new ZLinkLocationOptions();
        var admissionLifetime = options.OwnerLeaseTtl - options.OwnerLeaseFencingMargin;
        await using var runtime = new ZLinkLocationRuntime(options, flaky, time);
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        time.AdvanceMonotonic(admissionLifetime - TimeSpan.FromSeconds(1));
        flaky.HoldRenew = true;
        var lateRenew = runtime.RenewOwnerLeaseOnceAsync().AsTask();
        await flaky.RenewStarted.Task;
        flaky.Fail = true;
        Assert.False(await runtime.RenewOwnerLeaseOnceAsync());
        var episode = runtime.GetHealthSnapshot().StoreOutage;
        time.AdvanceMonotonic(TimeSpan.FromSeconds(1));
        Assert.True(runtime.GetHealthSnapshot().StoreOutage!.Discarded);
        flaky.ReleaseRenew.TrySetResult();
        Assert.True(await lateRenew);
        Assert.True(runtime.IsOwnerAdmissionOpen);
        Assert.Equal(episode! with { Discarded = true }, runtime.GetHealthSnapshot().StoreOutage);
    }

    [Fact]
    public async Task StoreOutageRetention_Missing_Read_And_Conflict_Are_Definitive()
    {
        var time = new ControllableTimeProvider();
        var flaky = new OutageLeaseStore(new ZLinkInMemoryLocationStore(time))
        {
            ConflictClaim = true,
            HoldRead = true,
        };
        await using var runtime = new ZLinkLocationRuntime(new ZLinkLocationOptions(), flaky, time);
        var confirmation = runtime.RenewOwnerLeaseOnceAsync().AsTask();
        await flaky.ReadStarted.Task;
        flaky.ConflictClaim = false;
        flaky.HoldRead = false;
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        flaky.MissingRead = true;
        flaky.ReleaseRead.TrySetResult();
        await Assert.ThrowsAsync<ZLinkOwnerLeaseClaimRejectedException>(() => confirmation);
        Assert.Null(runtime.GetHealthSnapshot().StoreOutage);
        Assert.Equal(0, time.ActiveTimerCount);
    }

    [Fact]
    public async Task StoreOutageRetention_General_Store_Read_Failure_Does_Not_Open_Episode()
    {
        var time = new ControllableTimeProvider();
        var services = new ServiceCollection();
        services.AddZLinkFramework(builder =>
        {
            builder.UseTestTimeProvider(time);
            builder.UseTestLocationStore();
        });
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkLocationRuntime>();
        var health = provider.GetRequiredService<ZLinkLocationStoreHealth>();
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        await Assert.ThrowsAsync<TimeoutException>(() =>
            ZLinkLocationStoreRead
                .ExecuteAsync<ZLinkOwnerLeaseReadResult>(
                    health,
                    "read",
                    default,
                    _ =>
                        ValueTask.FromException<ZLinkOwnerLeaseReadResult>(
                            new TimeoutException("read failed")
                        )
                )
                .AsTask()
        );
        Assert.Null(runtime.GetHealthSnapshot().StoreOutage);
        Assert.Equal(0, time.ActiveTimerCount);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task StoreOutageRetention_Uses_Request_Start_And_Injected_Timeout(
        bool provider,
        bool retention
    )
    {
        var time = new ControllableTimeProvider();
        IZLinkLocationRepository store = provider
            ? new ZLinkProviderLocationRepository(new ZLinkInMemoryProviderLocationStore(time))
            : new ZLinkInMemoryLocationStore(time);
        var flaky = new OutageLeaseStore(store) { HoldClaim = true };
        var options = new ZLinkLocationOptions { StoreOutageRetention = retention };
        await using var runtime = new ZLinkLocationRuntime(options, flaky, time);
        var initialClaim = runtime.RenewOwnerLeaseOnceAsync().AsTask();
        await flaky.ClaimStarted.Task;
        time.AdvanceMonotonic(TimeSpan.FromSeconds(2));
        flaky.ReleaseClaim.TrySetResult();
        Assert.True(await initialClaim);
        flaky.HoldRenew = true;
        var renew = runtime.RenewOwnerLeaseOnceAsync().AsTask();
        await flaky.RenewStarted.Task;
        time.AdvanceMonotonic(TimeSpan.FromSeconds(3));
        Assert.False(await renew);
        var episode = runtime.GetHealthSnapshot().StoreOutage;
        Assert.NotNull(episode);
        Assert.Equal(0, episode.LeaseOperationStartedAt);
        Assert.Equal(TimeSpan.FromSeconds(5).Ticks, episode.ObservedAt);
        var cutoff = retention
            ? options.OwnerLeaseTtl
                + options.OwnerLeaseFencingMargin
                + options.OwnerLeaseFencingMargin
            : options.OwnerLeaseTtl - options.OwnerLeaseFencingMargin;
        Assert.Equal(cutoff, episode.Lifetime);
        time.AdvanceMonotonic(cutoff - TimeSpan.FromSeconds(5) - TimeSpan.FromTicks(1));
        Assert.False(runtime.GetHealthSnapshot().StoreOutage!.Discarded);
        time.AdvanceMonotonic(TimeSpan.FromTicks(1));
        Assert.True(runtime.GetHealthSnapshot().StoreOutage!.Discarded);
        flaky.ReleaseRenew.TrySetResult();
    }

    [Fact]
    public async Task StoreOutageRetention_Builder_Option_Is_Frozen_At_Startup()
    {
        var services = new ServiceCollection();
        ZLinkLocationOptions configured = null!;
        services.AddZLinkFramework(builder =>
        {
            builder.UseTestTimeProvider(new ControllableTimeProvider());
            builder.UseTestLocationStore();
            configured = builder.ConfigureLocations();
            Assert.False(configured.StoreOutageRetention);
            configured.StoreOutageRetention = true;
        });
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkLocationRuntime>();
        await runtime.StartAsync(RoutingId.From("retention-startup"));
        Assert.True(configured.StoreOutageRetention);
        Assert.Throws<InvalidOperationException>(() => configured.StoreOutageRetention = false);
        await runtime.StopAsync();
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task StoreOutageRetention_Deadline_Does_Not_Inherit_Owner_Lane(bool provider)
    {
        var time = new ContextCapturingTimeProvider();
        IZLinkLocationRepository store = provider
            ? new ZLinkProviderLocationRepository(new ZLinkInMemoryProviderLocationStore(time))
            : new ZLinkInMemoryLocationStore(time);
        var flaky = new OutageLeaseStore(store);
        var options = new ZLinkLocationOptions();
        var admissionLifetime = options.OwnerLeaseTtl - options.OwnerLeaseFencingMargin;
        await using var runtime = new ZLinkLocationRuntime(options, flaky, time);
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        flaky.Fail = true;
        Assert.False(await runtime.RenewOwnerLeaseOnceAsync());
        time.Clock.AdvanceMonotonic(admissionLifetime);
        Assert.True(runtime.GetHealthSnapshot().StoreOutage!.Discarded);
    }

    [Theory]
    [InlineData(false, false, ZLinkUserSpotExecutionMode.PerActor)]
    [InlineData(false, false, ZLinkUserSpotExecutionMode.SpotWide)]
    [InlineData(false, true, ZLinkUserSpotExecutionMode.PerActor)]
    [InlineData(false, true, ZLinkUserSpotExecutionMode.SpotWide)]
    [InlineData(true, false, ZLinkUserSpotExecutionMode.PerActor)]
    [InlineData(true, false, ZLinkUserSpotExecutionMode.SpotWide)]
    [InlineData(true, true, ZLinkUserSpotExecutionMode.PerActor)]
    [InlineData(true, true, ZLinkUserSpotExecutionMode.SpotWide)]
    public async Task StoreOutageExecution_T_Discards_Published_Spot_Without_A_Turn(
        bool providerStore,
        bool retention,
        ZLinkUserSpotExecutionMode mode
    )
    {
        var time = new ControllableTimeProvider();
        IZLinkLocationRepository store = providerStore
            ? new ZLinkProviderLocationRepository(new ZLinkInMemoryProviderLocationStore(time))
            : new ZLinkInMemoryLocationStore(time);
        var flaky = new OutageLeaseStore(store);
        var probe = new OutageExecutionProbe();
        var services = new ServiceCollection().AddSingleton(probe);
        ZLinkLocationOptions options = null!;
        services.AddZLinkFramework(builder =>
        {
            builder.DisableImplicitHandlerAutoRegistration();
            builder.UseTestLocationStore();
            builder.AddRelocationStore(new InMemoryRelocationStore());
            builder.UseTestTimeProvider(time);
            options = builder.ConfigureLocations();
            options.StoreOutageRetention = retention;
            var server = builder
                .AddRouteMesh("outage-execution")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server();
            server.AddSpotFactory<OutageExecutionSpot>(
                "outage-execution",
                factory => factory.ExecutionMode(mode).RecreateOnRelocation()
            );
            server.AddEntrySpot<OutageExecutionEntry>();
            server.AddActorFactory<OutageExecutionActor, OutageExecutionActorFactory>(
                "outage-actor",
                factory => factory.DisableRelocation()
            );
        });
        services.AddSingleton<IZLinkLocationRepository>(flaky);
        services.AddSingleton(sp => new ZLinkLocationRuntime(options, flaky, time));
        await using var servicesProvider = services.BuildServiceProvider();
        servicesProvider.GetRequiredService<ZLinkFrameworkRegistration>().Locations._repository =
            flaky;
        var runtime = servicesProvider.GetRequiredService<ZLinkFrameworkRuntime>();
        var locations = servicesProvider.GetRequiredService<ZLinkLocationRuntime>();
        var hosted = servicesProvider
            .GetServices<IHostedService>()
            .Single(service => service is ZLinkFrameworkHostedService);
        await hosted.StartAsync(CancellationToken.None);
        Task? running = null;
        Task<ZLinkActorCreateResult>? pendingCreation = null;
        Task? closing = null;
        ZLinkActorRuntimeState? moving = null;
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        try
        {
            await servicesProvider
                .GetRequiredService<IZLinkSpotManager>()
                .GetOrCreate("steady", "outage-execution")
                .InMesh("outage-execution")
                .Async();
            var activation = Assert.Single(runtime.GetSpotNodeRuntime("outage-execution").Spots);
            var spot = Assert.IsType<OutageExecutionSpot>(activation.Spot);
            var actors = servicesProvider.GetRequiredService<IZLinkActorManager>();
            await actors.Create("steady-actor", "outage-actor").InMesh("outage-execution").Async();
            Assert.True(runtime.TryGetActorState("steady-actor", out var actor));
            actor.JoinSpot(activation);
            var closingResult = await servicesProvider
                .GetRequiredService<IZLinkSpotManager>()
                .GetOrCreate("closing", "outage-execution")
                .InMesh("outage-execution")
                .Async();
            var closingActivation = runtime
                .GetSpotNodeRuntime("outage-execution")
                .Spots.Single(item => item.SpotId == "closing");
            closing = servicesProvider
                .GetRequiredService<IZLinkSpotManager>()
                .CloseAsync(closingResult.Spot)
                .AsTask();
            await probe.ClosingEntered.Task;
            await actors.Create("lost-actor", "outage-actor").InMesh("outage-execution").Async();
            Assert.True(runtime.TryGetActorState("lost-actor", out var lostActor));
            lostActor.JoinSpot(activation);
            await actors.Create("moving", "outage-actor").InMesh("outage-execution").Async();
            Assert.True(runtime.TryGetActorState("moving", out moving));
            moving.Handoff.BeginCapture();
            await actors.Create("moving-member", "outage-actor").InMesh("outage-execution").Async();
            Assert.True(runtime.TryGetActorState("moving-member", out var movingMember));
            movingMember.JoinSpot(activation);
            movingMember.Handoff.BeginCapture();
            pendingCreation = actors
                .Create("late", "outage-actor")
                .InMesh("outage-execution")
                .Async()
                .AsTask();
            await probe.CreatingEntered.Task;
            Assert.True(runtime.TryGetActorState("late", out var lateActor));
            await actors.Create("direct-actor", "outage-actor").InMesh("outage-execution").Async();
            var rows = servicesProvider.GetRequiredService<ZLinkStoreLocationResolvers>();
            var directRoute = Assert.IsType<ZLinkResolvedActorLocation>(
                await rows.ResolveActorRowAsync(new ZLinkActorLocationKey("direct-actor"))
            );
            var client = new ZLinkActorClient(runtime);
            async ValueTask<OutageExecutionReply> PingActor() =>
                await client
                    .RequestToActorExact(
                        directRoute.MeshName,
                        directRoute.ActorRef,
                        directRoute.OwnerNodeGeneration,
                        directRoute.AuthorityOwnerGeneration,
                        checked((ulong)directRoute.LeaseGeneration),
                        "ping"
                    )
                    .Async<OutageExecutionReply>();
            async ValueTask<OutageExecutionReply> PingSpot() =>
                await new ZLinkInstanceSpotRequestCall<string>(
                    runtime,
                    "steady",
                    "ping"
                ).Async<OutageExecutionReply>();
            Assert.Equal(1, (await PingActor()).Count);
            Assert.Equal(1, (await PingSpot()).Count);
            Assert.NotNull(await rows.ResolveSpotRowAsync(new ZLinkSpotLocationKey("steady")));
            time.AdvanceMonotonic(TimeSpan.FromSeconds(1));
            flaky.Fail = true;
            Assert.False(await locations.RenewOwnerLeaseOnceAsync());
            var episode = locations.GetHealthSnapshot().StoreOutage!;
            Assert.Contains(actor, episode.Executions);
            Assert.Contains(activation, episode.Executions);
            Assert.DoesNotContain(lateActor, episode.Executions);
            Assert.DoesNotContain(moving, episode.Executions);
            Assert.DoesNotContain(movingMember, episode.Executions);
            Assert.DoesNotContain(closingActivation, episode.Executions);
            probe.ReleaseClosing.TrySetResult();
            await closing;
            probe.ReleaseCreating.TrySetResult();
            await pendingCreation;
            Assert.NotNull(lateActor.Actor);
            lateActor.JoinSpot(activation);
            var remaining = episode.Lifetime - time.GetElapsedTime(episode.LeaseOperationStartedAt);
            time.AdvanceMonotonic(remaining - TimeSpan.FromTicks(1));
            Assert.False(locations.GetHealthSnapshot().StoreOutage!.Discarded);
            var messages = 0;
            await actor.ExecuteDispatchAsync(
                _ =>
                {
                    messages++;
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            );
            Assert.Equal(1, messages);
            Assert.Equal(2, (await PingActor()).Count);
            Assert.Equal(2, (await PingSpot()).Count);
            var timer = Assert.IsType<ZLinkTimer>(spot.Timer);
            await spot.FirstTick.Task;
            Assert.Equal(1, spot.Ticks);
            if (retention)
            {
                Assert.False(locations.IsOwnerAdmissionOpen);
                Assert.False(lateActor.IsLocationExecutionAllowed);
                await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                    lateActor
                        .ExecuteDispatchAsync(
                            _ =>
                                throw new InvalidOperationException(
                                    "An unretained Actor must not enter its handler."
                                ),
                            CancellationToken.None
                        )
                        .AsTask()
                );
                var calls = probe.FactoryCalls;
                await Assert.ThrowsAsync<InvalidOperationException>(() =>
                    actors
                        .Create("denied", "outage-actor")
                        .InMesh("outage-execution")
                        .Async()
                        .AsTask()
                );
                Assert.Equal(calls, probe.FactoryCalls);
                Assert.NotNull(await rows.ResolveSpotRowAsync(new ZLinkSpotLocationKey("steady")));
                await Assert.ThrowsAsync<InvalidOperationException>(() =>
                    rows.ResolveSpotRowAsync(new ZLinkSpotLocationKey("uncached")).AsTask()
                );
                Assert.Throws<InvalidOperationException>(locations.EnsureRelocationAdmissionOpen);
                using var operation = (
                    await runtime.TryAdmitInboundOperationAsync(countAsRequest: false)
                ).Enter();
                await Assert.ThrowsAsync<InvalidOperationException>(() =>
                    runtime
                        .GetOrCreateAsync<OutageExecutionSpot>("new", ZLinkMessage.Empty)
                        .AsTask()
                );
            }
            locations.NotifyAuthorityOwnershipLost(
                ZLinkLocationKind.Actor,
                ZLinkActorAuthorityPayloadCodec.AuthorityKey("lost-actor")
            );
            Assert.True(lostActor.ContextInvalidated);
            Assert.False(activation.IsDisposed);
            Assert.False(actor.ContextInvalidated);
            await runtime.DiscardLocationExecutionAsync(lostActor);
            var record = new OutageExecutionRecord(lostActor);
            await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                activation
                    ._serial.ExecuteActorAsync(
                        "lost-actor",
                        static (_, item, _) =>
                        {
                            item.HandlerCalls++;
                            return ValueTask.CompletedTask;
                        },
                        record,
                        CancellationToken.None
                    )
                    .AsTask()
            );
            Assert.Equal(0, record.HandlerCalls);
            var entered = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            running = actor
                .ExecuteDispatchAsync(
                    async _ =>
                    {
                        entered.TrySetResult();
                        await release.Task;
                    },
                    CancellationToken.None
                )
                .AsTask();
            await entered.Task;
            var queued = actor
                .ExecuteDispatchAsync(
                    _ =>
                    {
                        messages++;
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None
                )
                .AsTask();
            time.AdvanceMonotonic(TimeSpan.FromTicks(1));
            Assert.True(locations.GetHealthSnapshot().StoreOutage!.Discarded);
            Assert.True(activation.IsDisposed);
            Assert.True(actor.ContextInvalidated);
            Assert.True(lateActor.ContextInvalidated);
            Assert.False(moving.ContextInvalidated);
            Assert.True(movingMember.ContextInvalidated);
            release.TrySetResult();
            await running;
            await Assert.ThrowsAsync<ZLinkFrameworkException>(() => queued);
            await runtime.DiscardLocationExecutionAsync(actor);
            await activation.DisposeAsync();
            Assert.Null(actor.Actor);
            Assert.Null(movingMember.Actor);
            Assert.True(timer.IsDisposed);
            Assert.Equal(1, messages);
            Assert.Equal(1, spot.Ticks);
            flaky.Fail = false;
            Assert.True(await locations.RenewOwnerLeaseOnceAsync());
            Assert.True(locations.IsOwnerAdmissionOpen);
            Assert.False(actor.IsLocationExecutionAllowed);
            Assert.False(activation.IsLocationExecutionAllowed);
            await timer.NotifyDueAsync(2);
            Assert.Equal(1, spot.Ticks);
        }
        finally
        {
            release.TrySetResult();
            probe.ReleaseCreating.TrySetResult();
            probe.ReleaseClosing.TrySetResult();
            if (closing is not null)
                await closing;
            if (pendingCreation is not null)
                await pendingCreation;
            if (running is not null)
                await running;
            if (locations.GetHealthSnapshot().StoreOutage is { Discarded: false } pending)
                time.AdvanceMonotonic(
                    pending.Lifetime - time.GetElapsedTime(pending.LeaseOperationStartedAt)
                );
            if (moving is not null)
                await runtime.DiscardLocationExecutionAsync(moving);
            flaky.Fail = false;
            await runtime.ForceStopAsync(CancellationToken.None);
            await locations.StopAsync();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task StoreOutageExecution_Definitive_Owner_Loss_Discards_Before_T(
        bool providerStore
    )
    {
        var time = new ControllableTimeProvider();
        IZLinkLocationRepository store = providerStore
            ? new ZLinkProviderLocationRepository(new ZLinkInMemoryProviderLocationStore(time))
            : new ZLinkInMemoryLocationStore(time);
        var flaky = new OutageLeaseStore(store);
        await using var locations = new ZLinkLocationRuntime(
            new ZLinkLocationOptions { StoreOutageRetention = true },
            flaky,
            time
        );
        await using var services = new ServiceCollection()
            .AddSingleton(locations)
            .BuildServiceProvider();
        var actor = new ZLinkActorRuntimeState("stale-owner", services: services);
        actor.BindActorInstance(new Runtime.EntrySpotActorDispatchTests.ProbeActor("stale-owner"));
        var creating = new ZLinkActorRuntimeState("creating-owner", services: services);
        var moving = new ZLinkActorRuntimeState("moving-owner", services: services);
        moving.BindActorInstance(
            new Runtime.EntrySpotActorDispatchTests.ProbeActor("moving-owner")
        );
        moving.Handoff.BeginCapture();
        locations.CaptureExecutions = () => new HashSet<object> { actor, creating, moving };
        ValueTask cleanup = default;
        locations.ExecutionsDiscarded += executions =>
        {
            Assert.Contains(creating, executions);
            Assert.Contains(moving, executions);
            cleanup = new ValueTask(
                Task.WhenAll(
                    executions
                        .OfType<ZLinkActorRuntimeState>()
                        .Select(state =>
                            state.InvalidateRuntimeGenerationAfterDispatchesAsync().AsTask()
                        )
                )
            );
        };
        Assert.True(await locations.RenewOwnerLeaseOnceAsync());
        var fenced = new ZLinkActorRuntimeState("already-fenced", services: services);
        fenced.BindActorInstance(
            new Runtime.EntrySpotActorDispatchTests.ProbeActor("already-fenced")
        );
        fenced.FenceRuntimeGeneration();
        Assert.False(fenced.IsLocationExecutionAllowed);
        await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            fenced
                .ExecuteDispatchAsync(
                    _ =>
                        throw new InvalidOperationException(
                            "A fenced Actor must not start a handler."
                        ),
                    CancellationToken.None
                )
                .AsTask()
        );
        await fenced.InvalidateRuntimeGenerationAfterDispatchesAsync();
        time.AdvanceMonotonic(TimeSpan.FromSeconds(1));
        flaky.Stale = true;
        Assert.False(await locations.RenewOwnerLeaseOnceAsync());
        Assert.Null(locations.GetHealthSnapshot().StoreOutage);
        Assert.False(locations.IsOwnerAdmissionOpen);
        Assert.True(actor.ContextInvalidated);
        Assert.True(creating.ContextInvalidated);
        Assert.True(moving.ContextInvalidated);
        await cleanup;
        Assert.Null(actor.Actor);
        flaky.Stale = false;
        Assert.True(await locations.RenewOwnerLeaseOnceAsync());
        Assert.False(actor.IsLocationExecutionAllowed);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task StoreOutageExecution_Late_Completion_Cannot_Resume_After_Recovery(
        bool providerStore,
        bool sharedGate
    )
    {
        var time = new ControllableTimeProvider();
        IZLinkLocationRepository store = providerStore
            ? new ZLinkProviderLocationRepository(new ZLinkInMemoryProviderLocationStore(time))
            : new ZLinkInMemoryLocationStore(time);
        var flaky = new OutageLeaseStore(store);
        await using var locations = new ZLinkLocationRuntime(
            new ZLinkLocationOptions { StoreOutageRetention = true },
            flaky,
            time
        );
        await using var services = new ServiceCollection()
            .AddSingleton(locations)
            .BuildServiceProvider();
        var actor = new ZLinkActorRuntimeState("late-completion", services: services);
        actor.BindActorInstance(
            new Runtime.EntrySpotActorDispatchTests.ProbeActor("late-completion")
        );
        locations.CaptureExecutions = () => new HashSet<object> { actor };
        ValueTask cleanup = default;
        locations.ExecutionsDiscarded += _ =>
            cleanup = actor.InvalidateRuntimeGenerationAfterDispatchesAsync();
        Assert.True(await locations.RenewOwnerLeaseOnceAsync());
        var owner = locations.OwnerToken;
        using var errors = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None, new object()),
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedGate: sharedGate,
            executionAllowed: () => actor.IsLocationExecutionAllowed
        );
        var entered = new TaskCompletionSource<ZLinkSerialTurn>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var reply = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var continuationRan = false;
        var item = await queue.PostAsync(
            async _ =>
            {
                var turn = Assert.IsType<ZLinkSerialTurn>(ZLinkSerialTurn.Current);
                entered.TrySetResult(turn);
                await turn.YieldFrameworkCallAsync(
                    _ => new ValueTask(reply.Task),
                    CancellationToken.None
                );
                continuationRan = true;
            },
            CancellationToken.None
        );
        try
        {
            await (await entered.Task).Suspended;
            time.AdvanceMonotonic(TimeSpan.FromSeconds(1));
            flaky.Fail = true;
            Assert.False(await locations.RenewOwnerLeaseOnceAsync());
            var episode = locations.GetHealthSnapshot().StoreOutage!;
            time.AdvanceMonotonic(
                episode.Lifetime - time.GetElapsedTime(episode.LeaseOperationStartedAt)
            );
            Assert.True(actor.ContextInvalidated);
            await cleanup;
            flaky.Fail = false;
            await locations.StartAsync(RoutingId.From("rejoined-after-outage"));
            Assert.True(locations.IsOwnerAdmissionOpen);
            Assert.NotEqual(owner, locations.OwnerToken);
            reply.TrySetResult();
            await Assert.ThrowsAsync<ZLinkFrameworkException>(() => item.Completion);
            Assert.False(continuationRan);
            Assert.Null(actor.Actor);
        }
        finally
        {
            reply.TrySetResult();
        }
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task StoreOutageReview_Turn_Checks_Once_Without_Reading_Clock(
        bool sharedGate,
        bool useRecord
    )
    {
        var time = new CountingTimeProvider();
        await using var locations = new ZLinkLocationRuntime(
            new ZLinkLocationOptions(),
            new ZLinkInMemoryLocationStore(time),
            time
        );
        await using var services = new ServiceCollection()
            .AddSingleton(locations)
            .BuildServiceProvider();
        var actor = new ZLinkActorRuntimeState("counted", services: services);
        actor.BindActorInstance(new Runtime.EntrySpotActorDispatchTests.ProbeActor("counted"));
        Assert.True(await locations.RenewOwnerLeaseOnceAsync());
        var delegateCalls = 0;
        var record = new CountedExecutionRecord(actor);
        using var errors = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None, new object()),
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedGate: sharedGate,
            executionAllowed: () =>
            {
                Interlocked.Increment(ref delegateCalls);
                return actor.IsLocationExecutionAllowed;
            }
        );
        time.ResetReads();
        var handlers = 0;
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostApplicationWithAdmission(
                _ =>
                {
                    handlers++;
                    return ValueTask.CompletedTask;
                },
                0,
                0,
                false,
                out var item,
                executionState: useRecord ? record : null
            )
        );
        await item.Completion;
        Assert.Equal(1, handlers);
        Assert.Equal(useRecord ? 0 : 1, delegateCalls);
        Assert.Equal(useRecord ? 1 : 0, record.Checks);
        Assert.Equal(0, time.Reads);
        // The counter also observes the admission path that must still read time.
        Assert.True(locations.IsOwnerAdmissionOpen);
        Assert.True(time.Reads > 0);
        await actor.InvalidateRuntimeGenerationAfterDispatchesAsync();
    }

    private sealed class CountedExecutionRecord(ZLinkActorRuntimeState actor)
        : IZLinkObjectExecutionRecord
    {
        internal int Checks;
        public bool IsExecutionAllowed
        {
            get
            {
                Interlocked.Increment(ref Checks);
                return actor.IsLocationExecutionAllowed;
            }
        }
    }

    private sealed class CountingTimeProvider : TimeProvider
    {
        private int _reads;
        internal int Reads => Volatile.Read(ref _reads);

        internal void ResetReads() => Interlocked.Exchange(ref _reads, 0);

        public override long TimestampFrequency => TimeSpan.TicksPerSecond;

        public override long GetTimestamp()
        {
            Interlocked.Increment(ref _reads);
            return 0;
        }

        public override DateTimeOffset GetUtcNow()
        {
            Interlocked.Increment(ref _reads);
            return DateTimeOffset.UnixEpoch;
        }

        public override ITimer CreateTimer(
            TimerCallback callback,
            object? state,
            TimeSpan dueTime,
            TimeSpan period
        ) => new ControllableTimeProvider().CreateTimer(callback, state, dueTime, period);
    }

    [Theory]
    [InlineData(false, ZLinkUserSpotExecutionMode.PerActor)]
    [InlineData(false, ZLinkUserSpotExecutionMode.SpotWide)]
    [InlineData(true, ZLinkUserSpotExecutionMode.PerActor)]
    [InlineData(true, ZLinkUserSpotExecutionMode.SpotWide)]
    public async Task StoreOutageReview_Location_Progresses_While_Spot_State_Lane_Is_Held(
        bool staleOwner,
        ZLinkUserSpotExecutionMode mode
    )
    {
        var time = new ControllableTimeProvider();
        var store = new OutageLeaseStore(new ZLinkInMemoryLocationStore(time));
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
                .AddRouteMesh("lane-review")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server();
            server.AddSpotFactory<OutageExecutionSpot>(
                "lane-review",
                factory => factory.ExecutionMode(mode).RecreateOnRelocation()
            );
            server.AddEntrySpot<OutageExecutionEntry>();
            server.AddActorFactory<OutageExecutionActor, OutageExecutionActorFactory>(
                "outage-actor",
                factory => factory.DisableRelocation()
            );
        });
        services.AddSingleton<IZLinkLocationRepository>(store);
        services.AddSingleton(sp => new ZLinkLocationRuntime(options, store, time));
        await using var provider = services.BuildServiceProvider();
        provider.GetRequiredService<ZLinkFrameworkRegistration>().Locations._repository = store;
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var locations = provider.GetRequiredService<ZLinkLocationRuntime>();
        var hosted = provider
            .GetServices<IHostedService>()
            .Single(service => service is ZLinkFrameworkHostedService);
        await hosted.StartAsync(CancellationToken.None);
        using var release = new ManualResetEventSlim();
        var held = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task? holding = null;
        try
        {
            await provider
                .GetRequiredService<IZLinkSpotManager>()
                .GetOrCreate("review", "lane-review")
                .InMesh("lane-review")
                .Async();
            var node = runtime.GetSpotNodeRuntime("lane-review");
            var spot = Assert.Single(node.Spots);
            await provider
                .GetRequiredService<IZLinkActorManager>()
                .Create("review-member", "outage-actor")
                .InMesh("lane-review")
                .Async();
            Assert.True(runtime.TryGetActorState("review-member", out var member));
            member.JoinSpot(spot);
            store.Fail = !staleOwner;
            store.Stale = staleOwner;
            if (!staleOwner)
            {
                Assert.False(await locations.RenewOwnerLeaseOnceAsync());
                Assert.Contains(spot, locations.GetHealthSnapshot().StoreOutage!.Executions);
            }
            holding = Task.Run(async () =>
                await spot._lane.RunAsync(() =>
                {
                    held.TrySetResult();
                    release.Wait();
                })
            );
            await held.Task;
            if (staleOwner)
                Assert.False(await locations.RenewOwnerLeaseOnceAsync());
            else
            {
                var episode = locations.GetHealthSnapshot().StoreOutage!;
                time.AdvanceMonotonic(
                    episode.Lifetime - time.GetElapsedTime(episode.LeaseOperationStartedAt)
                );
                Assert.True(locations.GetHealthSnapshot().StoreOutage!.Discarded);
            }
            // This lease attempt needs the Location lane while the Spot lane is held.
            store.Fail = true;
            store.Stale = false;
            Assert.False(await locations.RenewOwnerLeaseOnceAsync());
            Assert.False(spot.IsLocationExecutionAllowed);
            Assert.False(member.IsLocationExecutionAllowed);
            release.Set();
            await holding;
            // Join the same finalization, including member teardown and the parent gate.
            await runtime.DiscardLocationExecutionAsync(spot);
            Assert.True(spot.IsDisposed);
            Assert.Null(member.Actor);
            Assert.Empty(node.Spots);
        }
        finally
        {
            release.Set();
            if (holding is not null)
                await holding;
            await runtime.ForceStopAsync(CancellationToken.None);
            await locations.StopAsync();
        }
    }

    private sealed class OutageExecutionSpot(IZLinkSpotContext context, OutageExecutionProbe probe)
        : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
        public IZLinkTimer? Timer { get; private set; }
        public int Ticks;
        public int Messages;

        public void Configure() => Context.Handlers.AddPacket<OutageExecutionSpotRequestHandler>();

        public TaskCompletionSource FirstTick { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async ValueTask OnClosingAsync(ZLinkSpotClosingContext closing, CancellationToken ct)
        {
            if (Context.SpotId == "closing")
            {
                probe.ClosingEntered.TrySetResult();
                await probe.ReleaseClosing.Task.WaitAsync(ct);
            }
        }

        public async ValueTask OnInitializeAsync(CancellationToken ct)
        {
            Timer = await Context.AddTimer<OutageExecutionTick>(
                "outage",
                TimeSpan.FromSeconds(12),
                cancellationToken: ct
            );
        }
    }

    private sealed class OutageExecutionTick : IZLinkSpotTimerHandler<OutageExecutionSpot>
    {
        public ValueTask HandleAsync(
            OutageExecutionSpot spot,
            ZLinkTimerTick tick,
            CancellationToken ct
        )
        {
            spot.Ticks++;
            spot.FirstTick.TrySetResult();
            return ValueTask.CompletedTask;
        }
    }

    private sealed class OutageExecutionActor(IZLinkActorContext context) : IZLinkActor
    {
        public IZLinkActorContext Context { get; } = context;
        public int Messages;
    }

    private sealed record OutageExecutionReply(int Count);

    private sealed class OutageExecutionSpotRequestHandler
        : IZLinkSpotRequestHandler<OutageExecutionSpot, string, OutageExecutionReply>
    {
        public ValueTask<OutageExecutionReply> HandleAsync(
            OutageExecutionSpot spot,
            string request,
            CancellationToken ct
        ) => ValueTask.FromResult(new OutageExecutionReply(++spot.Messages));
    }

    private sealed class OutageExecutionActorRequestHandler
        : IZLinkEntrySpotActorRequestHandler<
            OutageExecutionEntry,
            OutageExecutionActor,
            string,
            OutageExecutionReply
        >
    {
        public ValueTask<OutageExecutionReply> HandleAsync(
            OutageExecutionEntry spot,
            OutageExecutionActor actor,
            IZLinkMessageContext context,
            string request,
            CancellationToken ct
        ) => ValueTask.FromResult(new OutageExecutionReply(++actor.Messages));
    }

    private sealed class OutageExecutionEntry(IZLinkEntrySpotContext context)
        : IZLinkEntrySpot<OutageExecutionActor>
    {
        public IZLinkEntrySpotContext Context { get; } = context;

        public void Configure() =>
            Context.Handlers.AddActorPacket<
                OutageExecutionActorRequestHandler,
                OutageExecutionActor
            >(nameof(String));

        public ValueTask OnJoinedActorAsync(OutageExecutionActor actor, CancellationToken ct) =>
            ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(OutageExecutionActor actor, CancellationToken ct) =>
            ValueTask.CompletedTask;
    }

    private sealed class OutageExecutionProbe
    {
        public int FactoryCalls;
        public TaskCompletionSource ClosingEntered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseClosing { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource CreatingEntered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseCreating { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class OutageExecutionRecord(ZLinkActorRuntimeState actor)
        : IZLinkObjectExecutionRecord
    {
        public int HandlerCalls;
        public bool IsExecutionAllowed => actor.IsLocationExecutionAllowed;
    }

    private sealed class OutageExecutionActorFactory(OutageExecutionProbe probe)
        : IZLinkActorFactory<OutageExecutionActor>
    {
        public async ValueTask<OutageExecutionActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken ct = default
        )
        {
            probe.FactoryCalls++;
            if (context.ActorId == "late")
            {
                probe.CreatingEntered.TrySetResult();
                await probe.ReleaseCreating.Task.WaitAsync(ct);
            }
            return new OutageExecutionActor(context);
        }
    }

    private sealed class ContextCapturingTimeProvider : TimeProvider
    {
        internal ControllableTimeProvider Clock { get; } = new();

        public override DateTimeOffset GetUtcNow() => Clock.GetUtcNow();

        public override long GetTimestamp() => Clock.GetTimestamp();

        public override long TimestampFrequency => Clock.TimestampFrequency;

        public override ITimer CreateTimer(
            TimerCallback callback,
            object? state,
            TimeSpan dueTime,
            TimeSpan period
        )
        {
            var context = ExecutionContext.Capture();
            return Clock.CreateTimer(
                _ =>
                {
                    if (context is null)
                        callback(state);
                    else
                        ExecutionContext.Run(context, _ => callback(state), null);
                },
                null,
                dueTime,
                period
            );
        }
    }

    private sealed class OutageLeaseStore(IZLinkLocationRepository inner)
        : ZLinkLocationStoreTestDouble
    {
        public override ValueTask<ZLinkLocationWriteResult> UpdateMeshNodeAsync(
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkLocationWriteIntent intent,
            CancellationToken cancellationToken = default
        ) => inner.UpdateMeshNodeAsync(descriptor, intent, cancellationToken);

        public override ValueTask<ZLinkLocationWriteStatus> RemoveMeshNodeAsync(
            ZLinkMeshNodeDescriptorKey key,
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default
        ) => inner.RemoveMeshNodeAsync(key, owner, cancellationToken);

        public override ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> ListMeshNodesAsync(
            string meshName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default
        ) => inner.ListMeshNodesAsync(meshName, page, cancellationToken);

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default
        ) => inner.ReadAuthorityAsync(key, cancellationToken);

        public override ValueTask<ZLinkAuthorityCompareExchangeResult> CompareExchangeAuthorityAsync(
            ZLinkAuthorityKey key,
            string expectedStoreVersion,
            ZLinkAuthorityMutation mutation,
            CancellationToken cancellationToken = default
        ) =>
            inner.CompareExchangeAuthorityAsync(
                key,
                expectedStoreVersion,
                mutation,
                cancellationToken
            );

        public override ValueTask<ZLinkAuthorityScanResult> ListAuthoritiesAsync(
            string prefix,
            ZLinkAuthorityScanCursor? cursor,
            int limit,
            CancellationToken cancellationToken = default
        ) => inner.ListAuthoritiesAsync(prefix, cursor, limit, cancellationToken);

        public override ValueTask<ZLinkObjectReserveResult> ReserveAsync(
            ZLinkObjectReservationRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ReserveAsync(request, cancellationToken);

        public override ValueTask<ZLinkObjectCommitResult> CommitAsync(
            ZLinkObjectReservation reservation,
            ReadOnlyMemory<byte> readyPayload,
            CancellationToken cancellationToken = default
        ) => inner.CommitAsync(reservation, readyPayload, cancellationToken);

        public override ValueTask<ZLinkObjectCreationCompleteResult> CompleteCreationAsync(
            ZLinkObjectReservation reservation,
            ZLinkObjectCreationCompletion completion,
            CancellationToken cancellationToken = default
        ) => inner.CompleteCreationAsync(reservation, completion, cancellationToken);

        public override ValueTask<ZLinkCreationTerminalReadResult> ReadCreationTerminalAsync(
            ZLinkCreationOperationId operation,
            CancellationToken cancellationToken = default
        ) => inner.ReadCreationTerminalAsync(operation, cancellationToken);

        public override ValueTask<ZLinkObjectAbortResult> AbortAsync(
            ZLinkObjectReservation reservation,
            CancellationToken cancellationToken = default
        ) => inner.AbortAsync(reservation, cancellationToken);

        public override ValueTask<long> RemoveAllByOwnerAsync(
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default
        ) => inner.RemoveAllByOwnerAsync(owner, cancellationToken);

        public override ValueTask<ulong?> GetMeshNodeChangeStampAsync(
            string meshName,
            CancellationToken cancellationToken = default
        ) => inner.GetMeshNodeChangeStampAsync(meshName, cancellationToken);

        internal bool Fail;
        internal bool Stale;
        internal bool HoldClaim;
        internal bool HoldRenew;
        internal bool ConflictClaim;
        internal bool HoldRead;
        internal bool FailRead;
        internal bool MissingRead;
        internal TaskCompletionSource ReadStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource ReleaseRead { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource RenewStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource ReleaseRenew { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public override async ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        )
        {
            if (HoldRead)
            {
                ReadStarted.TrySetResult();
                await ReleaseRead.Task;
            }
            if (FailRead)
                throw new InvalidOperationException("Provider read failed");
            if (MissingRead)
                return new ZLinkOwnerLeaseReadResult.Missing();
            return await inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);
        }

        internal TaskCompletionSource ClaimStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource ReleaseClaim { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public override async ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        )
        {
            if (ConflictClaim)
                return new ZLinkOwnerLeaseClaimResult.Conflict();
            var result = await inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl, cancellationToken);
            if (HoldClaim)
            {
                ClaimStarted.TrySetResult();
                await ReleaseClaim.Task;
            }
            return result;
        }

        public override async ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        )
        {
            if (Fail)
                throw new TimeoutException("Store unavailable");
            if (Stale)
                return new ZLinkOwnerLeaseRenewResult.Stale();
            var result = await inner.RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken);
            if (HoldRenew)
            {
                RenewStarted.TrySetResult();
                await ReleaseRenew.Task;
            }
            return result;
        }
    }

    [Fact]
    public async Task ForceStop2646_Expired_Stop_Closes_Local_Heartbeat_Without_Store_Work()
    {
        var store = new ZLinkInMemoryLocationStore();
        var ownerStore = new CancelingOwnerCleanupStore(store);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions
            {
                OwnerLeaseRenewInterval = TimeSpan.FromHours(1),
                OwnerLeaseTtl = TimeSpan.FromHours(2),
            },
            ownerStore
        );
        await runtime.StartAsync(RoutingId.From("expired-stop"));
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            runtime.StopAsync(cancellation.Token).AsTask()
        );
        Assert.False(runtime.GetHealthSnapshot().Healthy);
        Assert.False(ownerStore.RemovalStarted.Task.IsCompleted);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(runtime.OwnerId)
        );
        await runtime.DisposeAsync();
    }

    [Fact]
    public async Task Owner_Lease_Renew_Tracks_Health_And_Recovers_After_Store_Failure()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var flaky = new FlakyOwnerLeaseStore(store);
        var runtime = NewRuntime(flaky, time);

        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.True(runtime.GetHealthSnapshot().Healthy);
        Assert.Null(runtime.LastError);

        // Fail-static: a store outage is recorded, never thrown, and the
        // next successful renew clears the error.
        flaky.Fail = true;
        Assert.False(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.False(runtime.GetHealthSnapshot().Healthy);
        Assert.NotNull(runtime.LastError);

        flaky.Fail = false;
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.True(runtime.GetHealthSnapshot().Healthy);
        Assert.Null(runtime.LastError);
    }

    [Fact]
    public async Task Owner_Admission_Uses_The_Monotonic_Fencing_Deadline()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions
        {
            OwnerLeaseTtl = TimeSpan.FromSeconds(15),
            OwnerLeaseFencingMargin = TimeSpan.FromSeconds(5),
        };
        var runtime = new ZLinkLocationRuntime(options, store, time);

        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.True(runtime.IsOwnerAdmissionOpen);
        var cleanupToken = runtime.OwnerToken;

        time.AdvanceMonotonicOnly(TimeSpan.FromSeconds(10));
        Assert.False(runtime.IsOwnerAdmissionOpen);
        Assert.Throws<InvalidOperationException>(() => _ = runtime.AdmissionOwnerToken);
        Assert.Equal(cleanupToken, runtime.OwnerToken);

        // A later exact-token renewal establishes a new shared deadline.
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.True(runtime.IsOwnerAdmissionOpen);

        // Wall-clock changes cannot lengthen or shorten the local fence.
        time.AdvanceWallClockOnly(TimeSpan.FromHours(1));
        Assert.True(runtime.IsOwnerAdmissionOpen);
    }

    [Fact]
    public async Task Owner_Lease_Claim_Timeout_RechecksAndInstallsTheConfirmedLease()
    {
        var store = new ZLinkInMemoryLocationStore();
        var ambiguous = new AmbiguousCommittedClaimStore(store);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions { OwnerLeaseRenewTimeout = TimeSpan.FromMilliseconds(25) },
            ambiguous
        );

        var renewed = await runtime
            .RenewOwnerLeaseOnceAsync()
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(1));

        Assert.True(renewed);
        Assert.Equal(1, ambiguous.ClaimCalls);
        Assert.Equal(1, ambiguous.ReadCalls);
        Assert.True(runtime.IsOwnerAdmissionOpen);
        Assert.Equal(runtime.OwnerId, runtime.OwnerToken.OwnerId);
    }

    [Fact]
    public async Task ClaimExhaustingTheRenewDeadline_DoesNotStartConfirmationRead()
    {
        var time = new ControllableTimeProvider();
        var timeout = TimeSpan.FromMilliseconds(25);
        var store = new NonCooperativeClaimStore();
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions { OwnerLeaseRenewTimeout = timeout },
            store,
            time
        );

        var renew = runtime.RenewOwnerLeaseOnceAsync().AsTask();
        Assert.False(renew.IsCompleted);
        Assert.Equal(0, store.ReadCalls);
        time.Advance(timeout);
        Assert.False(await renew.WaitAsync(TimeSpan.FromSeconds(1)));
        Assert.Equal(0, store.ReadCalls);
        Assert.Equal(0, time.ActiveTimerCount);
    }

    [Fact]
    public async Task Tokenless_ClaimConflict_RechecksAndConvergesOnTheSameOwnerLease()
    {
        var store = new ZLinkInMemoryLocationStore();
        var ambiguous = new AmbiguousCommittedClaimStore(store, hiddenReads: 1);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions { OwnerLeaseRenewTimeout = TimeSpan.FromMilliseconds(25) },
            ambiguous
        );

        Assert.False(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.False(runtime.IsOwnerAdmissionOpen);

        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.Equal(2, ambiguous.ClaimCalls);
        Assert.Equal(2, ambiguous.ReadCalls);
        Assert.True(runtime.IsOwnerAdmissionOpen);
        Assert.Equal(runtime.OwnerId, runtime.OwnerToken.OwnerId);
    }

    [Fact]
    public async Task Startup_ClaimTimeout_CompletesWithoutAnAdmissionDeadline()
    {
        var store = new ZLinkInMemoryLocationStore();
        var hanging = new HangingOwnerLeaseStore(store);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions
            {
                OwnerLeaseRenewTimeout = TimeSpan.FromMilliseconds(25),
                OwnerLeaseRenewInterval = TimeSpan.FromHours(1),
            },
            hanging
        );

        await runtime
            .StartAsync(RoutingId.From("timeout-node"))
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(1));

        Assert.False(runtime.IsOwnerAdmissionOpen);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await store.ReadOwnerLeaseAsync(runtime.OwnerId)
        );
        await runtime.StopAsync();
    }

    [Fact]
    public async Task Authority_Reservation_Race_Gives_One_Winner_Across_Runtimes()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var runtimeA = NewRuntime(store, time);
        var runtimeB = NewRuntime(store, time);
        await runtimeA.RenewOwnerLeaseOnceAsync();
        await runtimeB.RenewOwnerLeaseOnceAsync();

        await RegisterObjectNodeAsync(runtimeA);
        var requestA = AuthorityReservation(
            runtimeA,
            "actor:play:actor-1",
            ZLinkPlacementObjectKind.Actor,
            "player"
        );
        var requestB = requestA with
        {
            CreationIntentReference = "intent:actor:play:actor-1:b",
            CreationIntentHash = SHA256.HashData(
                System.Text.Encoding.UTF8.GetBytes("intent:actor:play:actor-1:b")
            ),
            TargetOwner = runtimeB.OwnerToken,
        };

        var winner = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(requestA)
        );
        var rejected = Assert.IsType<ZLinkObjectReserveResult.Conflict>(
            await store.ReserveAsync(requestB)
        );
        var current = Assert.IsType<ZLinkAuthorityReadResult.Found>(rejected.Current).Snapshot;
        Assert.Equal(ZLinkPlacementAllocationState.Reserved, current.Allocation.State);
        Assert.Equal(runtimeA.OwnerId, current.OwnerId);
        var committed = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(winner.Reservation, new byte[] { 0x21 })
        );

        Assert.Equal(runtimeA.OwnerId, committed.Snapshot.OwnerId);
    }

    [Fact]
    public async Task Authority_Cas_Rejects_A_Stale_Writer_After_A_Newer_Commit()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var runtime = NewRuntime(store, time);
        await runtime.RenewOwnerLeaseOnceAsync();
        await RegisterObjectNodeAsync(runtime);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                AuthorityReservation(
                    runtime,
                    "actor:play:actor-1",
                    ZLinkPlacementObjectKind.Actor,
                    "player"
                )
            )
        );
        var committed = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(reserved.Reservation, new byte[] { 0x31 })
        );
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await store.CompareExchangeAuthorityAsync(
                reserved.Reservation.Key,
                committed.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    new byte[] { 0x32 },
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null
                )
            )
        );

        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Conflict>(
            await store.CompareExchangeAuthorityAsync(
                reserved.Reservation.Key,
                committed.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    new byte[] { 0x33 },
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null
                )
            )
        );
    }

    [Fact]
    public async Task Shutdown_Removes_Owner_Lease_Then_Bulk_Removes_Rows()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var runtime = NewRuntime(store, time);
        await runtime.StartAsync(RoutingId.From("node-1"));
        await RegisterObjectNodeAsync(runtime);
        await CommitAuthorityAsync(
            runtime,
            "actor:play:actor-1",
            ZLinkPlacementObjectKind.Actor,
            "player"
        );
        await CommitAuthorityAsync(
            runtime,
            "spot:play:spot-1",
            ZLinkPlacementObjectKind.UserSpot,
            "game"
        );

        await runtime.StopAsync();

        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await store.ReadOwnerLeaseAsync(runtime.OwnerId)
        );
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await store.ReadAuthorityAsync(new ZLinkAuthorityKey("actor:play:actor-1"))
        );
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await store.ReadAuthorityAsync(new ZLinkAuthorityKey("spot:play:spot-1"))
        );
        Assert.Single((await store.ListMeshNodesAsync("play", default)).Items);
    }

    [Fact]
    public async Task StopAsync_Propagates_Caller_Cancellation_And_Remains_Safe_To_Dispose()
    {
        var inner = new ZLinkInMemoryLocationStore();
        var ownerStore = new CancelingOwnerCleanupStore(inner);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions
            {
                OwnerLeaseRenewInterval = TimeSpan.FromHours(1),
                OwnerLeaseTtl = TimeSpan.FromHours(2),
            },
            ownerStore
        );
        await runtime.StartAsync(RoutingId.From("cancel-stop-node"));
        using var cancellation = new CancellationTokenSource();

        var stop = runtime.StopAsync(cancellation.Token).AsTask();
        await ownerStore.RemovalStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => stop);
        Assert.False(runtime.GetHealthSnapshot().Healthy);
        Assert.Equal(0, ownerStore.RenewCalls);

        await runtime.StopAsync(CancellationToken.None);
        await runtime.DisposeAsync();
        Assert.Equal(0, ownerStore.RenewCalls);
    }

    [Fact]
    public async Task Drain_Cleanup_Failure_Keeps_Lease_Heartbeat_Until_Retry_Succeeds()
    {
        var inner = new ZLinkInMemoryLocationStore();
        var store = new FailOnceOwnerCleanupStore(inner);
        var options = new ZLinkLocationOptions
        {
            OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(10),
            OwnerLeaseTtl = TimeSpan.FromSeconds(15),
        };
        var runtime = new ZLinkLocationRuntime(options, store);
        await runtime.StartAsync(RoutingId.From("node-1"));

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.CleanupOwnerForDrainAsync(CancellationToken.None)
        );
        await store.HeartbeatAfterFailure.Task.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await inner.ReadOwnerLeaseAsync(runtime.OwnerId)
        );

        await runtime.CleanupOwnerForDrainAsync(CancellationToken.None);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await inner.ReadOwnerLeaseAsync(runtime.OwnerId)
        );
        await runtime.StopAsync();
    }

    [Fact]
    public async Task Drain_Cleanup_Removes_Every_Owner_Row_And_The_Owner_Lease()
    {
        var store = new ZLinkInMemoryLocationStore();
        var runtime = new ZLinkLocationRuntime(new ZLinkLocationOptions(), store);
        await runtime.StartAsync(RoutingId.From("node-drain"));
        await RegisterObjectNodeAsync(runtime);
        await CommitAuthorityAsync(
            runtime,
            "actor:play:actor-1",
            ZLinkPlacementObjectKind.Actor,
            "player"
        );
        await CommitAuthorityAsync(
            runtime,
            "spot:play:spot-drain",
            ZLinkPlacementObjectKind.UserSpot,
            "game"
        );

        await runtime.CleanupOwnerForDrainAsync(CancellationToken.None);

        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await store.ReadOwnerLeaseAsync(runtime.OwnerId)
        );
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await store.ReadAuthorityAsync(new ZLinkAuthorityKey("actor:play:actor-1"))
        );
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await store.ReadAuthorityAsync(new ZLinkAuthorityKey("spot:play:spot-drain"))
        );
        Assert.Single((await store.ListMeshNodesAsync("play", default)).Items);
        await runtime.StopAsync();
    }

    [Fact]
    public async Task Startup_WithoutAnOwnerLease_BlocksAdmission_AndHeartbeatClaimsAfterRecovery()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var flaky = new FlakyOwnerLeaseStore(store) { Fail = true };
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions
            {
                OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(5),
                OwnerLeaseRenewTimeout = TimeSpan.FromMilliseconds(25),
            },
            flaky,
            time
        );

        await runtime.StartAsync(RoutingId.From("node-1"));
        Assert.False(runtime.IsOwnerAdmissionOpen);
        Assert.Throws<InvalidOperationException>(() => _ = runtime.AdmissionOwnerToken);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await store.ReadOwnerLeaseAsync(runtime.OwnerId)
        );

        var renewed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        runtime.OwnerLeaseRenewed += _ => renewed.TrySetResult();
        flaky.Fail = false;
        await renewed.Task.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.True(runtime.IsOwnerAdmissionOpen);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(runtime.OwnerId)
        );
        await runtime.StopAsync();
    }

    [Fact]
    public async Task Startup_ClaimConflict_IsAnError()
    {
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions(),
            new TerminalClaimStore(new ZLinkOwnerLeaseClaimResult.Conflict())
        );

        await Assert.ThrowsAsync<ZLinkOwnerLeaseClaimRejectedException>(() =>
            runtime.StartAsync(RoutingId.From("conflict-node")).AsTask()
        );
    }

    [Fact]
    public async Task Startup_ClaimConflict_RemainsAnErrorWhenConfirmationReadTimesOut()
    {
        var store = new ConflictWithNonCooperativeReadStore();
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions { OwnerLeaseRenewTimeout = TimeSpan.FromMilliseconds(25) },
            store
        );

        await Assert.ThrowsAsync<ZLinkOwnerLeaseClaimRejectedException>(() =>
            runtime
                .StartAsync(RoutingId.From("conflict-read-timeout-node"))
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(1))
        );
        Assert.Equal(1, store.ReadCalls);
    }

    [Fact]
    public async Task Startup_ClaimGenerationExhausted_IsAnError()
    {
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions(),
            new TerminalClaimStore(new ZLinkOwnerLeaseClaimResult.GenerationExhausted())
        );

        await Assert.ThrowsAsync<ZLinkOwnerLeaseClaimRejectedException>(() =>
            runtime.StartAsync(RoutingId.From("exhausted-node")).AsTask()
        );
    }

    [Fact]
    public async Task Startup_Cancellation_ReleasesACommittedClaim_AndDoesNotStartHeartbeat()
    {
        var time = new ControllableTimeProvider();
        var inner = new ZLinkInMemoryLocationStore(time);
        var store = new CancelAfterCommittedClaimStore(inner);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions
            {
                OwnerLeaseRenewTimeout = TimeSpan.FromSeconds(1),
                OwnerLeaseRenewInterval = TimeSpan.FromHours(1),
            },
            store,
            time
        );
        using var cancellation = new CancellationTokenSource();

        var start = runtime
            .StartAsync(RoutingId.From("cancelled-claim-node"), cancellation.Token)
            .AsTask();
        await store.ClaimStarted.Task.WaitAsync(TimeSpan.FromSeconds(1));
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            start.WaitAsync(TimeSpan.FromMilliseconds(500))
        );
        Assert.Equal(0, time.ActiveTimerCount);
        Assert.Equal(1, store.ReadCalls);
        Assert.Equal(1, store.ReleaseCalls);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await inner.ReadOwnerLeaseAsync(store.ClaimOwnerId!)
        );
        Assert.Equal(0, store.RenewCalls);
    }

    [Fact]
    public async Task Startup_Cancellation_PreservesTheOriginalExceptionWhenReleaseIgnoresCancellation()
    {
        var inner = new ZLinkInMemoryLocationStore();
        var store = new CancelAfterCommittedClaimWithHangingReleaseStore(inner);
        // The renew deadline runs on this clock, so the caller's cancellation
        // reaches the runtime before the deadline (spec 01-location-runtime
        // startup: a deadline that ends first completes startup).
        var time = new ControllableTimeProvider();
        var renewTimeout = TimeSpan.FromMilliseconds(25);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions
            {
                OwnerLeaseRenewTimeout = renewTimeout,
                OwnerLeaseRenewInterval = TimeSpan.FromHours(1),
            },
            store,
            time
        );
        using var cancellation = new CancellationTokenSource();

        var start = runtime
            .StartAsync(RoutingId.From("cancelled-hanging-release-node"), cancellation.Token)
            .AsTask();
        await store.ClaimStarted.Task.WaitAsync(TimeSpan.FromSeconds(1));
        var startedAt = Stopwatch.GetTimestamp();
        cancellation.Cancel();
        // The release ignores cancellation and ends at the cleanup deadline,
        // which is the startup renew deadline.
        await store.ReleaseStarted.Task.WaitAsync(TimeSpan.FromSeconds(1));
        time.AdvanceMonotonic(renewTimeout);

        var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            start.WaitAsync(TimeSpan.FromSeconds(1))
        );
        Assert.True(error.CancellationToken.IsCancellationRequested);
        Assert.True(Stopwatch.GetElapsedTime(startedAt) < TimeSpan.FromMilliseconds(500));
        Assert.Equal(1, store.ReleaseCalls);
        Assert.Contains("cleanup failed", runtime.LastError, StringComparison.OrdinalIgnoreCase);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await inner.ReadOwnerLeaseAsync(store.ClaimOwnerId!)
        );
        Assert.Equal(0, store.RenewCalls);
    }

    [Fact]
    public async Task Startup_CancellationAfterTheRenewTimeout_ReleasesTheConfirmedClaim()
    {
        var inner = new ZLinkInMemoryLocationStore();
        var store = new CommittedClaimUntilCancelledStore(inner);
        var time = new ControllableTimeProvider();
        var renewTimeout = TimeSpan.FromMilliseconds(25);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions
            {
                OwnerLeaseRenewTimeout = renewTimeout,
                OwnerLeaseRenewInterval = TimeSpan.FromHours(1),
            },
            store,
            time
        );
        using var cancellation = new CancellationTokenSource();
        // The caller cancels after the renew timeout ended the claim attempt.
        runtime.OwnerLeaseRenewalFailed += cancellation.Cancel;

        var start = runtime
            .StartAsync(
                RoutingId.From("cancelled-after-timeout-node"),
                cancellation.Token,
                CancellationToken.None
            )
            .AsTask();
        await store.ClaimStarted.Task.WaitAsync(TimeSpan.FromSeconds(1));
        time.AdvanceMonotonic(renewTimeout);

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            start.WaitAsync(TimeSpan.FromSeconds(1))
        );
        Assert.Equal(1, store.ReleaseCalls);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await inner.ReadOwnerLeaseAsync(store.ClaimOwnerId!)
        );
        Assert.Equal(0, store.RenewCalls);
    }

    // Commits the claim, then answers only when the claim request is cancelled.
    private sealed class CommittedClaimUntilCancelledStore(IZLinkLocationRepository inner)
        : ZLinkLocationStoreTestDouble
    {
        private int _releaseCalls;
        private int _renewCalls;

        public TaskCompletionSource ClaimStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public string? ClaimOwnerId { get; private set; }

        public int ReleaseCalls => Volatile.Read(ref _releaseCalls);

        public int RenewCalls => Volatile.Read(ref _renewCalls);

        public override async ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        )
        {
            ClaimOwnerId = ownerId;
            _ = await inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl);
            ClaimStarted.TrySetResult();
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            throw new InvalidOperationException("The claim answers only by cancellation.");
        }

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        ) => inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        )
        {
            Interlocked.Increment(ref _renewCalls);
            return inner.RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken);
        }

        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default
        )
        {
            Interlocked.Increment(ref _releaseCalls);
            return inner.ReleaseOwnerLeaseAsync(token, cancellationToken);
        }
    }

    [Fact]
    public async Task Restart_UsesFreshOwnerGeneration_AndReportsStoppedLeaseUnhealthy()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var runtime = NewRuntime(store, time);

        await runtime.StartAsync(RoutingId.From("stable-node"));
        var firstOwner = runtime.OwnerId;
        Assert.True(runtime.GetHealthSnapshot().Healthy);

        await runtime.StopAsync();
        Assert.False(runtime.GetHealthSnapshot().Healthy);

        await runtime.StartAsync(RoutingId.From("stable-node"));
        Assert.NotEqual(firstOwner, runtime.OwnerId);
        Assert.True(runtime.GetHealthSnapshot().Healthy);
        await runtime.StopAsync();
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task FirstHeartbeat_IsDueFromTheInitialLeaseOperationStart(bool claimFails)
    {
        var options = new ZLinkLocationOptions
        {
            OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(100),
        };
        var time = new HeartbeatTimeProvider(options.OwnerLeaseRenewTimeout);
        await using var runtime = new ZLinkLocationRuntime(
            options,
            new FlakyOwnerLeaseStore(new ZLinkInMemoryLocationStore(time)) { Fail = claimFails },
            time
        );
        runtime.OwnerLeaseRenewed += _ => time.Timestamp = 50;
        runtime.OwnerLeaseRenewalFailed += () => time.Timestamp = 50;

        await runtime.StartAsync(RoutingId.From("first-heartbeat-node"));

        var first = await time.FirstTimer.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(TimeSpan.FromMilliseconds(50), first.DueTime);
    }

    [Fact]
    public async Task Heartbeat_UsesActualStartAfterLateTimer_AndWaitsForInFlightRenewal()
    {
        var options = new ZLinkLocationOptions
        {
            OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(100),
        };
        var time = new HeartbeatTimeProvider(options.OwnerLeaseRenewTimeout);
        var inner = new ZLinkInMemoryLocationStore(time);
        var store = new BlockingHeartbeatStore(inner, blockedRenewCall: 1);
        var runtime = new ZLinkLocationRuntime(options, store, time);
        try
        {
            await runtime.StartAsync(RoutingId.From("late-heartbeat-node"));
            var first = await time.FirstTimer.Task.WaitAsync(TimeSpan.FromSeconds(5));
            time.Timestamp = 150;
            first.Fire();
            await store.HeartbeatStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(time.SecondTimer.Task.IsCompleted);
            Assert.Equal(1, store.RenewCalls);
            time.Timestamp = 200;
            store.ReleaseHeartbeat.TrySetResult();
            var second = await time.SecondTimer.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(TimeSpan.FromMilliseconds(50), second.DueTime);
        }
        finally
        {
            store.ReleaseHeartbeat.TrySetResult();
            await runtime.DisposeAsync();
        }
    }

    [Fact]
    public async Task DisposeAsync_WaitsForInFlightHeartbeatBeforeCleaningOwnerResources()
    {
        var inner = new ZLinkInMemoryLocationStore();
        var store = new BlockingHeartbeatStore(inner);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions
            {
                OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(5),
                OwnerLeaseTtl = TimeSpan.FromSeconds(15),
            },
            store
        );
        await runtime.StartAsync(RoutingId.From("dispose-node"));
        await store.HeartbeatStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var firstDispose = runtime.DisposeAsync().AsTask();
        var secondDispose = runtime.DisposeAsync().AsTask();
        await Task.Delay(25);
        Assert.False(firstDispose.IsCompleted);
        Assert.False(secondDispose.IsCompleted);

        store.ReleaseHeartbeat.TrySetResult();
        await Task.WhenAll(firstDispose, secondDispose).WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(2, store.RenewCalls);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await inner.ReadOwnerLeaseAsync(runtime.OwnerId)
        );
    }

    [Fact]
    public async Task DisposeAsync_PreventsQueuedStartFromCreatingANewRuntimeGeneration()
    {
        var inner = new ZLinkInMemoryLocationStore();
        var store = new BlockingHeartbeatStore(inner);
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions
            {
                OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(5),
                OwnerLeaseTtl = TimeSpan.FromSeconds(15),
            },
            store
        );
        await runtime.StartAsync(RoutingId.From("dispose-race-node"));
        await store.HeartbeatStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var stop = runtime.StopAsync().AsTask();
        await Task.Delay(25);
        Assert.False(stop.IsCompleted);
        var queuedStart = runtime.StartAsync(RoutingId.From("forbidden-generation")).AsTask();
        var dispose = runtime.DisposeAsync().AsTask();
        var repeatedDispose = runtime.DisposeAsync().AsTask();
        Assert.Same(dispose, repeatedDispose);

        store.ReleaseHeartbeat.TrySetResult();
        await stop.WaitAsync(TimeSpan.FromSeconds(5));
        await Assert.ThrowsAsync<ObjectDisposedException>(() => queuedStart);
        await dispose.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(2, store.RenewCalls);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await inner.ReadOwnerLeaseAsync(runtime.OwnerId)
        );
        await Assert.ThrowsAsync<ObjectDisposedException>(async () =>
            await runtime.StartAsync(RoutingId.From("after-dispose"))
        );
    }

    [Fact]
    public async Task SynchronousServiceProviderDispose_CannotSilentlyPartiallyDisposeLocationRuntime()
    {
        var store = new ZLinkInMemoryLocationStore();
        var runtime = new ZLinkLocationRuntime(new ZLinkLocationOptions(), store);
        var provider = new ServiceCollection().AddSingleton(_ => runtime).BuildServiceProvider();
        _ = provider.GetRequiredService<ZLinkLocationRuntime>();

        var failure = Assert.Throws<InvalidOperationException>(provider.Dispose);
        Assert.Contains("IAsyncDisposable", failure.Message, StringComparison.Ordinal);

        await runtime.DisposeAsync();
    }

    [Fact]
    public async Task InMemory_Registration_Resolves_Resolvers_Query_And_Shared_Store()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options => options.UseTestLocationStore());
        await using var provider = services.BuildServiceProvider();

        Assert.NotNull(provider.GetRequiredService<IZLinkMeshNodeLocationResolver>());
        Assert.NotNull(provider.GetRequiredService<ZLinkLocationAddressResolvers>());
        Assert.NotNull(provider.GetRequiredService<IZLinkLocationRuntimeQuery>());

        // The unified store is registered as one shared service instance.
        var first = provider.GetRequiredService<IZLinkLocationRepository>();
        var second = provider.GetRequiredService<IZLinkLocationRepository>();
        Assert.Same(first, second);
    }

    private static async ValueTask RegisterObjectNodeAsync(ZLinkLocationRuntime runtime)
    {
        var descriptor = InMemoryLocationStoreTests.MeshNode(
            runtime.OwnerId,
            nodeRid: "node-1"
        ) with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            EntrySpotId = "node-1-entry-test",
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.Actor,
                    "player",
                    ZLinkObjectMaintenancePolicyKind.Recreate,
                    HasSnapshotAdapter: false,
                    Limit: 0
                ),
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.UserSpot,
                    "game",
                    ZLinkObjectMaintenancePolicyKind.Recreate,
                    HasSnapshotAdapter: false,
                    Limit: 0
                ),
            ],
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, 0),
                new ZLinkPopulationCapacity(0, 0, 0),
                [new ZLinkSpotTypeCapacity(ZLinkPlacementObjectKind.UserSpot, "game", 0, 0, 0)]
            ),
        };
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (
                await runtime.WriteDescriptorAsync(descriptor, ZLinkLocationWriteIntent.NewClaim)
            ).Status
        );
    }

    private static ZLinkObjectReservationRequest AuthorityReservation(
        ZLinkLocationRuntime runtime,
        string key,
        ZLinkPlacementObjectKind kind,
        string stableType
    )
    {
        var encoded = System.Text.Encoding.UTF8.GetBytes(key);
        return new ZLinkObjectReservationRequest(
            kind,
            new ZLinkAuthorityKey(key),
            stableType,
            $"intent:{key}",
            SHA256.HashData(encoded),
            encoded.Length,
            new ZLinkMeshNodeDescriptorKey("play", RoutingId.From("node-1")),
            1,
            runtime.OwnerToken,
            new byte[] { 0x10 },
            kind == ZLinkPlacementObjectKind.Actor
                ? new ZLinkCapacityVector(1, 0, null)
                : new ZLinkCapacityVector(0, 1, new ZLinkSpotTypeCapacityDelta(kind, stableType, 1))
        );
    }

    private static async ValueTask CommitAuthorityAsync(
        ZLinkLocationRuntime runtime,
        string key,
        ZLinkPlacementObjectKind kind,
        string stableType
    )
    {
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await runtime.Store.ReserveAsync(AuthorityReservation(runtime, key, kind, stableType))
        );
        Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await runtime.Store.CommitAsync(reserved.Reservation, new byte[] { 0x20 })
        );
    }

    private static ZLinkLocationRuntime NewRuntime(
        IZLinkLocationRepository store,
        ManualTimeProvider time
    ) => new(new ZLinkLocationOptions(), store, time);

    private sealed class FlakyOwnerLeaseStore(IZLinkLocationRepository inner)
        : ZLinkLocationStoreTestDouble
    {
        public bool Fail { get; set; }

        public override async ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        )
        {
            if (Fail)
                throw new InvalidOperationException("store unreachable");
            return await inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl, cancellationToken);
        }

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        ) => inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        ) =>
            Fail
                ? throw new InvalidOperationException("store unreachable")
                : inner.RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default
        ) => inner.ReleaseOwnerLeaseAsync(token, cancellationToken);
    }

    private sealed class TerminalClaimStore(ZLinkOwnerLeaseClaimResult result)
        : ZLinkLocationStoreTestDouble
    {
        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        ) => ValueTask.FromResult(result);

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        ) =>
            ValueTask.FromResult<ZLinkOwnerLeaseReadResult>(
                new ZLinkOwnerLeaseReadResult.Missing()
            );
    }

    private sealed class ConflictWithNonCooperativeReadStore : ZLinkLocationStoreTestDouble
    {
        private readonly TaskCompletionSource<ZLinkOwnerLeaseReadResult> _read = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private int _readCalls;

        public int ReadCalls => Volatile.Read(ref _readCalls);

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        ) =>
            ValueTask.FromResult<ZLinkOwnerLeaseClaimResult>(
                new ZLinkOwnerLeaseClaimResult.Conflict()
            );

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        )
        {
            Interlocked.Increment(ref _readCalls);
            return new ValueTask<ZLinkOwnerLeaseReadResult>(_read.Task);
        }
    }

    private sealed class CancelAfterCommittedClaimStore(IZLinkLocationRepository inner)
        : ZLinkLocationStoreTestDouble
    {
        private readonly TaskCompletionSource<ZLinkOwnerLeaseClaimResult> _claim = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private int _readCalls;
        private int _releaseCalls;
        private int _renewCalls;

        public TaskCompletionSource ClaimStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public string? ClaimOwnerId { get; private set; }

        public int ReleaseCalls => Volatile.Read(ref _releaseCalls);

        public int ReadCalls => Volatile.Read(ref _readCalls);

        public int RenewCalls => Volatile.Read(ref _renewCalls);

        public override async ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        )
        {
            ClaimOwnerId = ownerId;
            _ = await inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl);
            ClaimStarted.TrySetResult();
            return await _claim.Task;
        }

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        )
        {
            Interlocked.Increment(ref _readCalls);
            return inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);
        }

        public override ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        )
        {
            Interlocked.Increment(ref _renewCalls);
            return inner.RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken);
        }

        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default
        )
        {
            Interlocked.Increment(ref _releaseCalls);
            return inner.ReleaseOwnerLeaseAsync(token, cancellationToken);
        }
    }

    private sealed class HangingOwnerLeaseStore(IZLinkLocationRepository inner)
        : ZLinkLocationStoreTestDouble
    {
        private readonly TaskCompletionSource<ZLinkOwnerLeaseClaimResult> claim = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private readonly TaskCompletionSource<ZLinkOwnerLeaseRenewResult> renewal = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        ) => new(claim.Task);

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        ) => inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        ) => new(renewal.Task);

        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default
        ) => inner.ReleaseOwnerLeaseAsync(token, cancellationToken);
    }

    private sealed class NonCooperativeClaimStore : ZLinkLocationStoreTestDouble
    {
        private readonly TaskCompletionSource<ZLinkOwnerLeaseClaimResult> _claim = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private readonly TaskCompletionSource<ZLinkOwnerLeaseReadResult> _read = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private int _readCalls;

        public int ReadCalls => Volatile.Read(ref _readCalls);

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        ) => new(_claim.Task);

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        )
        {
            Interlocked.Increment(ref _readCalls);
            return new ValueTask<ZLinkOwnerLeaseReadResult>(_read.Task);
        }
    }

    private sealed class CancelAfterCommittedClaimWithHangingReleaseStore(
        IZLinkLocationRepository inner
    ) : ZLinkLocationStoreTestDouble
    {
        private readonly TaskCompletionSource<ZLinkOwnerLeaseClaimResult> _claim = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private readonly TaskCompletionSource<ZLinkOwnerLeaseReleaseResult> _release = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private int _releaseCalls;
        private int _renewCalls;

        public TaskCompletionSource ClaimStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public string? ClaimOwnerId { get; private set; }

        public TaskCompletionSource ReleaseStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int ReleaseCalls => Volatile.Read(ref _releaseCalls);

        public int RenewCalls => Volatile.Read(ref _renewCalls);

        public override async ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        )
        {
            ClaimOwnerId = ownerId;
            _ = await inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl);
            ClaimStarted.TrySetResult();
            return await _claim.Task;
        }

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        ) => inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        )
        {
            Interlocked.Increment(ref _renewCalls);
            return inner.RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken);
        }

        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default
        )
        {
            Interlocked.Increment(ref _releaseCalls);
            ReleaseStarted.TrySetResult();
            return new ValueTask<ZLinkOwnerLeaseReleaseResult>(_release.Task);
        }
    }

    private sealed class AmbiguousCommittedClaimStore(
        IZLinkLocationRepository inner,
        int hiddenReads = 0
    ) : ZLinkLocationStoreTestDouble
    {
        private int _claimCalls;
        private int _readCalls;

        public int ClaimCalls => Volatile.Read(ref _claimCalls);

        public int ReadCalls => Volatile.Read(ref _readCalls);

        public override async ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        )
        {
            var call = Interlocked.Increment(ref _claimCalls);
            var result = await inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl, cancellationToken);
            if (call == 1)
                throw new TimeoutException("claim response was lost");
            return result;
        }

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        )
        {
            var call = Interlocked.Increment(ref _readCalls);
            return call <= hiddenReads
                ? ValueTask.FromResult<ZLinkOwnerLeaseReadResult>(
                    new ZLinkOwnerLeaseReadResult.Missing()
                )
                : inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);
        }
    }

    // Captures the heartbeat delay timers. Renew deadlines run on the same
    // clock with the renew timeout as due time; they are not heartbeat timers.
    private sealed class HeartbeatTimeProvider(TimeSpan renewTimeout) : TimeProvider
    {
        private int _timerCount;
        public long Timestamp { get; set; }
        public override long TimestampFrequency => 1000;

        public override long GetTimestamp() => Timestamp;

        public override DateTimeOffset GetUtcNow() => DateTimeOffset.UnixEpoch;

        public TaskCompletionSource<HeartbeatTimer> FirstTimer { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource<HeartbeatTimer> SecondTimer { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public override ITimer CreateTimer(
            TimerCallback callback,
            object? state,
            TimeSpan dueTime,
            TimeSpan period
        )
        {
            var timer = new HeartbeatTimer(callback, state, dueTime);
            if (dueTime == renewTimeout)
                return timer;
            if (Interlocked.Increment(ref _timerCount) == 1)
                FirstTimer.TrySetResult(timer);
            else
                SecondTimer.TrySetResult(timer);
            return timer;
        }
    }

    private sealed class HeartbeatTimer(TimerCallback callback, object? state, TimeSpan dueTime)
        : ITimer
    {
        public TimeSpan DueTime { get; } = dueTime;

        public void Fire() => callback(state);

        public bool Change(TimeSpan dueTime, TimeSpan period) => false;

        public void Dispose() { }

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private sealed class BlockingHeartbeatStore(
        IZLinkLocationRepository inner,
        int blockedRenewCall = 2
    ) : ZLinkLocationStoreTestDouble
    {
        private int _renewCalls;

        public TaskCompletionSource HeartbeatStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource ReleaseHeartbeat { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int RenewCalls => Volatile.Read(ref _renewCalls);

        public override ValueTask<long> RemoveAllByOwnerAsync(
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default
        ) => inner.RemoveAllByOwnerAsync(owner, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        ) => inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        ) => inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);

        public override async ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        )
        {
            if (Interlocked.Increment(ref _renewCalls) == blockedRenewCall)
            {
                HeartbeatStarted.TrySetResult();
                await ReleaseHeartbeat.Task.ConfigureAwait(false);
            }

            return await inner
                .RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken)
                .ConfigureAwait(false);
        }

        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default
        ) => inner.ReleaseOwnerLeaseAsync(token, cancellationToken);
    }

    private sealed class CancelingOwnerCleanupStore(IZLinkLocationRepository inner)
        : ZLinkLocationStoreTestDouble
    {
        private int _renewCalls;

        public TaskCompletionSource RemovalStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int RenewCalls => Volatile.Read(ref _renewCalls);

        public override ValueTask<long> RemoveAllByOwnerAsync(
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default
        ) => inner.RemoveAllByOwnerAsync(owner, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        ) => inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        ) => inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        )
        {
            Interlocked.Increment(ref _renewCalls);
            return inner.RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken);
        }

        public override async ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default
        )
        {
            RemovalStarted.TrySetResult();
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken).ConfigureAwait(false);
            return ZLinkOwnerLeaseReleaseResult.Stale;
        }
    }

    private sealed class FailOnceOwnerCleanupStore(IZLinkLocationRepository inner)
        : ZLinkLocationStoreTestDouble
    {
        private int _fail = 1;
        private int _renewals;

        public TaskCompletionSource HeartbeatAfterFailure { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public override ValueTask<long> RemoveAllByOwnerAsync(
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default
        ) =>
            Interlocked.Exchange(ref _fail, 0) != 0
                ? throw new InvalidOperationException("owner cleanup unavailable")
                : inner.RemoveAllByOwnerAsync(owner, cancellationToken);

        public override ValueTask<ZLinkLocationWriteResult> UpdateMeshNodeAsync(
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkLocationWriteIntent intent,
            CancellationToken cancellationToken = default
        ) => inner.UpdateMeshNodeAsync(descriptor, intent, cancellationToken);

        public override ValueTask<ZLinkLocationWriteStatus> RemoveMeshNodeAsync(
            ZLinkMeshNodeDescriptorKey key,
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default
        ) => inner.RemoveMeshNodeAsync(key, owner, cancellationToken);

        public override ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> ListMeshNodesAsync(
            string meshName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default
        ) => inner.ListMeshNodesAsync(meshName, page, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        ) => inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        ) => inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        )
        {
            if (Interlocked.Increment(ref _renewals) >= 2)
                HeartbeatAfterFailure.TrySetResult();
            return inner.RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken);
        }

        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default
        ) => inner.ReleaseOwnerLeaseAsync(token, cancellationToken);

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default
        ) => inner.ReadAuthorityAsync(key, cancellationToken);

        public override ValueTask<ZLinkAuthorityCompareExchangeResult> CompareExchangeAuthorityAsync(
            ZLinkAuthorityKey key,
            string expectedStoreVersion,
            ZLinkAuthorityMutation mutation,
            CancellationToken cancellationToken = default
        ) =>
            inner.CompareExchangeAuthorityAsync(
                key,
                expectedStoreVersion,
                mutation,
                cancellationToken
            );

        public override ValueTask<ZLinkAuthorityScanResult> ListAuthoritiesAsync(
            string prefix,
            ZLinkAuthorityScanCursor? cursor,
            int limit,
            CancellationToken cancellationToken = default
        ) => inner.ListAuthoritiesAsync(prefix, cursor, limit, cancellationToken);

        public override ValueTask<ZLinkObjectReserveResult> ReserveAsync(
            ZLinkObjectReservationRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ReserveAsync(request, cancellationToken);

        public override ValueTask<ZLinkObjectCommitResult> CommitAsync(
            ZLinkObjectReservation reservation,
            ReadOnlyMemory<byte> readyPayload,
            CancellationToken cancellationToken = default
        ) => inner.CommitAsync(reservation, readyPayload, cancellationToken);

        public override ValueTask<ZLinkObjectCreationCompleteResult> CompleteCreationAsync(
            ZLinkObjectReservation reservation,
            ZLinkObjectCreationCompletion completion,
            CancellationToken cancellationToken = default
        ) => inner.CompleteCreationAsync(reservation, completion, cancellationToken);

        public override ValueTask<ZLinkCreationTerminalReadResult> ReadCreationTerminalAsync(
            ZLinkCreationOperationId operation,
            CancellationToken cancellationToken = default
        ) => inner.ReadCreationTerminalAsync(operation, cancellationToken);

        public override ValueTask<ZLinkObjectAbortResult> AbortAsync(
            ZLinkObjectReservation reservation,
            CancellationToken cancellationToken = default
        ) => inner.AbortAsync(reservation, cancellationToken);

        public override ValueTask<ZLinkAggregatePrepareResult> PrepareAggregateAsync(
            ZLinkAggregatePrepareRequest request,
            CancellationToken cancellationToken = default
        ) => inner.PrepareAggregateAsync(request, cancellationToken);

        public override ValueTask<ZLinkAggregateCommitResult> CommitAggregateAsync(
            ZLinkAggregateFence fence,
            CancellationToken cancellationToken = default
        ) => inner.CommitAggregateAsync(fence, cancellationToken);

        public override ValueTask<ZLinkAggregateAbortResult> AbortAggregateAsync(
            ZLinkAggregateFence fence,
            CancellationToken cancellationToken = default
        ) => inner.AbortAggregateAsync(fence, cancellationToken);
    }
}
