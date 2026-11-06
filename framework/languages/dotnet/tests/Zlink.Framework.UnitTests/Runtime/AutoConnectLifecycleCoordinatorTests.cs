using System.Runtime.CompilerServices;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.UnitTests.Runtime;

namespace Zlink.Framework.UnitTests;

public sealed class AutoConnectLifecycleCoordinatorTests
{
    private static readonly TimeSpan TestTimeout = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task FrameworkPhaseOwnsExactlyOneGeneration()
    {
        var starts = 0;
        var stops = 0;
        var coordinator = CreateCoordinator(() => starts++, () => stops++);
        var state = UninitializedState();

        await coordinator.FrameworkReadyAsync(state, static () => { }, CancellationToken.None);
        await coordinator.FrameworkReadyAsync(state, static () => { }, CancellationToken.None);
        await coordinator.StopAsync(CancellationToken.None);
        await coordinator.StopAsync(CancellationToken.None);

        Assert.Equal(1, starts);
        Assert.Equal(1, stops);
    }

    [Fact]
    public async Task Expired_stop_observes_active_discovery_before_returning()
    {
        CancellationObservingResolver? resolver = null;
        await using var host = await RelocationBehaviorHost.StartAsync(
            "expired-discovery-stop",
            new RelocationBehaviorTrace(),
            new ZLinkInMemoryProviderLocationStore(),
            new SynchronizedRelocationStore(),
            registerTargetSpot: false,
            meshResolverWrapper: inner => resolver = new CancellationObservingResolver(inner)
        );
        resolver!.HoldReads = true;
        try
        {
            await resolver.ReadEntered.Task.WaitAsync(TestTimeout);
            using var deadline = new CancellationTokenSource();
            deadline.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                host
                    .Services.GetRequiredService<ZLinkLocationAutoConnectHost>()
                    .StopAsync(deadline.Token)
                    .AsTask()
            );
            Assert.True(resolver.ReadExited.Task.IsCompleted);
            var remaining = await host
                .Services.GetRequiredService<ZLinkStoreLocationResolvers>()
                .ListLiveMeshNodesAsync(RelocationBehaviorHost.MeshName);
            Assert.Contains(remaining, row => row.Rid == host.LocalNodeRid);
        }
        finally
        {
            resolver.ReleaseRead.TrySetResult();
        }
    }

    internal sealed class CancellationObservingResolver(IZLinkMeshNodeLocationResolver inner)
        : IZLinkMeshNodeLocationResolver
    {
        internal volatile bool HoldReads;
        internal TaskCompletionSource ReadEntered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource ReadExited { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource ReleaseRead { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
            string meshName,
            CancellationToken cancellationToken = default
        )
        {
            if (HoldReads)
            {
                ReadEntered.TrySetResult();
                try
                {
                    await ReleaseRead.Task.WaitAsync(cancellationToken);
                }
                finally
                {
                    ReadExited.TrySetResult();
                }
            }
            return await inner.ListLiveMeshNodesAsync(meshName, cancellationToken);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Runtime_restart_does_not_retain_location_generation(bool force)
    {
        using var host = new HostBuilder()
            .ConfigureServices(services =>
                services.AddZLinkFramework(options =>
                {
                    options.UseTestLocationStore();
                    options.AddRouteMesh("generation-reset").Listen("tcp://127.0.0.1:0");
                })
            )
            .Build();
        await host.StartAsync();
        var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        var locations = host.Services.GetRequiredService<ZLinkLocationLifecycle>();
        var spot = ZLinkSpotId.FromBoundary("old-entry", "spot");
        try
        {
            await locations.SpotLocations.ClaimAsync(
                ZLinkMeshName.FromBoundary("generation-reset", "mesh"),
                spot,
                17,
                null,
                RoutingId.From("generation-owner"),
                1,
                ZLinkSpotKind.Entry,
                1,
                null
            );
            Assert.Equal(17UL, await locations.SpotLocations.GetTrackedGenerationAsync(spot));
            if (force)
                await runtime.ForceStopAsync(CancellationToken.None);
            else
                await runtime.StopAsync(CancellationToken.None);
            await runtime.StartAsync(CancellationToken.None);
            Assert.Null(await locations.SpotLocations.GetTrackedGenerationAsync(spot));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
            await host.StopAsync();
        }
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task Host_stop_preserves_the_termination_result_after_cleanup_failure(
        bool cancelStop
    )
    {
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var host = new HostBuilder()
            .ConfigureServices(services =>
            {
                services.AddZLinkFramework(options =>
                {
                    options.UseTestLocationStore();
                    options.AddRouteMesh("stop-owner").Listen("tcp://127.0.0.1:0");
                });
                services.AddSingleton(provider =>
                {
                    var autoConnect = provider.GetRequiredService<ZLinkLocationAutoConnectHost>();
                    return new ZLinkAutoConnectLifecycleCoordinator(
                        autoConnect.StartAsync,
                        async token =>
                        {
                            entered.TrySetResult();
                            await release.Task.WaitAsync(token);
                            if (!cancelStop)
                                throw new InvalidOperationException("Generation cleanup failed.");
                            await autoConnect.StopAsync(token);
                        }
                    );
                });
            })
            .Build();
        await host.StartAsync();
        try
        {
            using var waiter = new CancellationTokenSource();
            var stop = host
                .Services.GetRequiredService<ZLinkAutoConnectLifecycleCoordinator>()
                .StopAsync(waiter.Token)
                .AsTask();
            await entered.Task.WaitAsync(TestTimeout);
            if (cancelStop)
            {
                waiter.Cancel();
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() => stop);
            }
            else
            {
                release.TrySetResult();
                await Assert.ThrowsAsync<InvalidOperationException>(() => stop);
            }
        }
        finally
        {
            release.TrySetResult();
        }
        await host.StopAsync().WaitAsync(TestTimeout);
        var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        var result = await host
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .ShutdownAsync();
        Assert.Equal(ZLinkFrameworkTerminationOutcome.ForceStopped, result.Outcome);
        if (!cancelStop)
            Assert.Equal(ZLinkFrameworkTerminationReason.TeardownFailed, result.Reason);
        Assert.False(runtime.IsStarted);
    }

    private static ZLinkAutoConnectLifecycleCoordinator CreateCoordinator(
        Action started,
        Action stopped
    )
    {
        return new ZLinkAutoConnectLifecycleCoordinator(
            (_, _, _, _) =>
            {
                started();
                return ValueTask.CompletedTask;
            },
            _ =>
            {
                stopped();
                return ValueTask.CompletedTask;
            }
        );
    }

    private static ZLinkFrameworkComponentState UninitializedState() =>
        (ZLinkFrameworkComponentState)
            RuntimeHelpers.GetUninitializedObject(typeof(ZLinkFrameworkComponentState));
}
