using System.Globalization;
using System.Text;
using System.Threading.Channels;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

/// <summary>
/// Location runtime §5 (startup): a MeshNode host whose Location Store answers
/// later than the renew timeout finishes startup without an admission
/// deadline, keeps claiming the owner lease and publishes its descriptor once
/// the lease is held. Only Conflict and GenerationExhausted end startup with
/// an error (issue #1962).
/// </summary>
public sealed class StartupStoreStallTests
{
    private const string MeshName = "stall-mesh";

    // Safety bound for a hung await only; no assertion depends on elapsed time.
    private static readonly TimeSpan Hang = TimeSpan.FromSeconds(30);

    // An Instance Spot factory adds the activation target, which also acts
    // for the owner lease.
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task MeshNodeHost_StoreHeldPastRenewTimeout_StartsThenPublishesServingDescriptor(
        bool instanceSpots
    )
    {
        var store = new GatedLocationStore(new ZLinkInMemoryProviderLocationStore());
        store.Hold();
        await using var provider = BuildProvider(store, instanceSpots);
        var hosted = HostedService(provider);
        var locations = provider.GetRequiredService<ZLinkLocationRuntime>();

        // Every Store response is still held: startup must not wait for it
        // and must not fail because the lease is not confirmed yet.
        try
        {
            await hosted.StartAsync(CancellationToken.None).WaitAsync(Hang);
            Assert.True(store.IsHolding);
            Assert.True(store.HeldCalls > 0);
            Assert.False(locations.IsOwnerAdmissionOpen);
            // §5: no new application work, so the host stays Preparing.
            var host = provider.GetRequiredService<IZLinkFrameworkRuntime>();
            Assert.Equal(ZLinkFrameworkRuntimeState.Preparing, host.Status.State);
            Assert.False(host.Status.IsReady);
            var repository = new ZLinkProviderLocationRepository(store.Inner);
            Assert.Empty((await repository.ListMeshNodesAsync(MeshName, default)).Items);

            store.Release();

            // The heartbeat keeps claiming; once it holds the lease the host
            // publishes its MeshNode descriptor and enters Serving.
            ZLinkMeshNodeDescriptor? descriptor = null;
            using var hang = new CancellationTokenSource(Hang);
            await foreach (var _ in store.CompletedWrites.Reader.ReadAllAsync(hang.Token))
            {
                descriptor = (
                    await repository.ListMeshNodesAsync(MeshName, default)
                ).Items.SingleOrDefault();
                if (descriptor is { State: ZLinkFrameworkRuntimeState.Serving })
                    break;
            }

            Assert.NotNull(descriptor);
            Assert.Equal(ZLinkFrameworkRuntimeState.Serving, descriptor.State);
            Assert.Equal(locations.OwnerId, descriptor.OwnerId);
            Assert.False(string.IsNullOrEmpty(descriptor.Endpoint));
            Assert.True(locations.IsOwnerAdmissionOpen);
            await foreach (var observed in host.ObserveAsync(hang.Token))
                if (observed.Status.State == ZLinkFrameworkRuntimeState.Serving)
                    break;
            Assert.True(host.Status.IsReady);
            Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
                await repository.ReadOwnerLeaseAsync(locations.OwnerId)
            );
        }
        finally
        {
            store.Release();
            await hosted.StopAsync(CancellationToken.None);
        }
    }

    // An Instance Spot factory adds the activation target, which also acts
    // for the owner lease.
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task MeshNodeHost_StoreUnavailableAtStartup_StartsAndStopsWithoutLease(
        bool instanceSpots
    )
    {
        var store = new GatedLocationStore(new ZLinkInMemoryProviderLocationStore())
        {
            Unavailable = true,
        };
        await using var provider = BuildProvider(store, instanceSpots);
        var hosted = HostedService(provider);
        var locations = provider.GetRequiredService<ZLinkLocationRuntime>();

        // Provider errors at startup leave the host without a lease; startup
        // still completes, and a host that never claimed stops cleanly.
        await hosted.StartAsync(CancellationToken.None).WaitAsync(Hang);
        Assert.False(locations.IsOwnerAdmissionOpen);
        Assert.Equal(
            ZLinkFrameworkRuntimeState.Preparing,
            provider.GetRequiredService<IZLinkFrameworkRuntime>().Status.State
        );
        await hosted.StopAsync(CancellationToken.None).WaitAsync(Hang);

        store.Unavailable = false;
        var repository = new ZLinkProviderLocationRepository(store.Inner);
        Assert.Empty((await repository.ListMeshNodesAsync(MeshName, default)).Items);
        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await repository.ReadOwnerLeaseAsync(locations.OwnerId)
        );
    }

    [Fact]
    public async Task MeshNodeHost_ServingDescriptorNotStored_FailsStartupWithWriteStatus()
    {
        // The lease is held, but the Store never accepts the Serving revision
        // of the claimed descriptor: startup fails and names the write status.
        var store = new GatedLocationStore(new ZLinkInMemoryProviderLocationStore())
        {
            RejectDescriptorRenew = true,
        };
        await using var provider = BuildProvider(store);

        var failure = await Assert.ThrowsAnyAsync<Exception>(() =>
            HostedService(provider).StartAsync(CancellationToken.None).WaitAsync(Hang)
        );
        Assert.Contains(
            "could not enter Serving state: IgnoredStale",
            failure.ToString(),
            StringComparison.Ordinal
        );
    }

    [Fact]
    public async Task MeshNodeHost_StartupClaimConflict_FailsStartup()
    {
        var store = new GatedLocationStore(new ZLinkInMemoryProviderLocationStore())
        {
            ReportOwnerLeaseOnce = true,
        };
        await using var provider = BuildProvider(store);

        await Assert.ThrowsAsync<ZLinkOwnerLeaseClaimRejectedException>(() =>
            HostedService(provider).StartAsync(CancellationToken.None).WaitAsync(Hang)
        );
    }

    [Fact]
    public async Task MeshNodeHost_StartupClaimGenerationExhausted_FailsStartup()
    {
        var store = new GatedLocationStore(new ZLinkInMemoryProviderLocationStore())
        {
            ExhaustOwnerGenerations = true,
        };
        await using var provider = BuildProvider(store);

        await Assert.ThrowsAsync<ZLinkOwnerLeaseClaimRejectedException>(() =>
            HostedService(provider).StartAsync(CancellationToken.None).WaitAsync(Hang)
        );
    }

    private static ServiceProvider BuildProvider(
        GatedLocationStore store,
        bool instanceSpots = false
    )
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.AddRelocationStore(new InMemoryRelocationStore());
            var locations = options.ConfigureLocations();
            locations.OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(20);
            // Long enough that a loaded machine cannot turn a prompt Store
            // answer into a timeout; a held Store still outlasts it.
            locations.OwnerLeaseRenewTimeout = TimeSpan.FromSeconds(1);
            locations.OwnerLeaseTtl = TimeSpan.FromSeconds(30);
            locations.OwnerLeaseFencingMargin = TimeSpan.FromSeconds(5);
            var server = options
                .AddRouteMesh(MeshName)
                .Listen("tcp://127.0.0.1:0")
                .SetRoutingIdPrefix("stall")
                .Objects()
                .Server();
            if (instanceSpots)
                server.AddInstanceSpotFactory<StallInstanceSpot>(
                    "stall.instance",
                    static factory => factory.DisableRelocation()
                );
        });
        return services.BuildServiceProvider();
    }

    private static IHostedService HostedService(IServiceProvider provider) =>
        provider
            .GetServices<IHostedService>()
            .Single(static service => service is ZLinkFrameworkHostedService);

    /// <summary>
    /// Provider-level store that holds every response while closed, the way a
    /// stalled Redis keeps a request pending regardless of cancellation.
    /// </summary>
    private sealed class GatedLocationStore(IZLinkLocationStore inner) : IZLinkLocationStore
    {
        private TaskCompletionSource _gate = Open();
        private int _heldCalls;
        private int _ownerLeaseReads;
        private bool _unavailable;

        internal IZLinkLocationStore Inner => inner;

        internal bool ReportOwnerLeaseOnce { get; init; }

        internal bool ExhaustOwnerGenerations { get; init; }

        internal bool RejectDescriptorRenew { get; init; }

        internal Channel<int> CompletedWrites { get; } = Channel.CreateUnbounded<int>();

        internal bool IsHolding => !Volatile.Read(ref _gate).Task.IsCompleted;

        internal int HeldCalls => Volatile.Read(ref _heldCalls);

        internal void Hold() =>
            Volatile.Write(
                ref _gate,
                new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously)
            );

        internal bool Unavailable
        {
            set => Volatile.Write(ref _unavailable, value);
        }

        internal void Release() => Volatile.Read(ref _gate).TrySetResult();

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            await PassGateAsync();
            if (
                ExhaustOwnerGenerations
                && key.Value.EndsWith("owner-counter", StringComparison.Ordinal)
            )
                return Found(long.MaxValue.ToString(CultureInfo.InvariantCulture));
            if (
                ReportOwnerLeaseOnce
                && key.Value.Contains("owner-lease", StringComparison.Ordinal)
                && Interlocked.Increment(ref _ownerLeaseReads) == 1
            )
                return Found("other-owner");
            return await inner.ReadAsync(key, cancellationToken);
        }

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            await PassGateAsync();
            if (
                RejectDescriptorRenew
                && request.Conditions.Any(condition =>
                    condition is ZLinkStoreCondition.Version version
                    && version.Key.Value.Contains("mesh-node", StringComparison.Ordinal)
                )
            )
                return new ZLinkStoreWriteResult.Conflict(DateTimeOffset.UtcNow);
            var result = await inner.WriteAsync(request, cancellationToken);
            CompletedWrites.Writer.TryWrite(0);
            return result;
        }

        public async ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        )
        {
            await PassGateAsync();
            return await inner.ScanAsync(request, cancellationToken);
        }

        private Task PassGateAsync()
        {
            if (Volatile.Read(ref _unavailable))
                return Task.FromException(
                    new InvalidOperationException("Location Store unavailable")
                );
            var gate = Volatile.Read(ref _gate).Task;
            if (gate.IsCompleted)
                return Task.CompletedTask;
            Interlocked.Increment(ref _heldCalls);
            return gate;
        }

        private static ZLinkStoreReadResult Found(string text) =>
            new ZLinkStoreReadResult.Found(
                new ZLinkStoreValue(
                    Encoding.UTF8.GetBytes(text),
                    new ZLinkStoreVersion("1"),
                    DateTimeOffset.UtcNow.AddMinutes(1),
                    DateTimeOffset.UtcNow
                )
            );

        private static TaskCompletionSource Open()
        {
            var gate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            gate.SetResult();
            return gate;
        }
    }
}

internal sealed class StallInstanceSpot(IZLinkInstanceSpotContext context) : IZLinkInstanceSpot
{
    public IZLinkInstanceSpotContext Context { get; } = context;
}
