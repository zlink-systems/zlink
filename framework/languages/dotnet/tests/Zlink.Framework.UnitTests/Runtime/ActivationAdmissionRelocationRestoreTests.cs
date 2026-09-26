using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Timers;

namespace Zlink.Framework.UnitTests;

/// <summary>
/// MeshNode §5.1: a relocation-target Restore holds one activation admission on the target
/// MeshNode from the moment the target receives it until target commit, or until the
/// aborted Restore is cleaned up. The shared fixture does not cover relocation, so this test
/// drives the prepared target Spot against the admission record directly.
/// </summary>
public sealed class ActivationAdmissionRelocationRestoreTests
{
    private const string StableType = "Tests.AdmissionRestoreInstanceSpot";

    [Fact]
    public async Task Relocation_target_restore_holds_one_admission_until_target_commit_or_cleanup()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options.AddRelocationStore(new InMemoryRelocationStore());
            options
                .AddRouteMesh("objects")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server()
                .AddInstanceSpotFactory<AdmissionRestoreInstanceSpot>(
                    StableType,
                    static factory => factory.DisableRelocation()
                );
        });

        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var locations = provider.GetRequiredService<ZLinkLocationRuntime>();
        await locations.StartAsync(runtime.PrepareLocationNodeRoutingId(), CancellationToken.None);
        ZLinkSpotNodeCatalog? catalog = null;
        ZLinkTimerScheduler? timerScheduler = null;
        try
        {
            await runtime.StartAsync(CancellationToken.None);
            var nodeRuntime = runtime.GetSpotNodeRuntime("objects");
            var admission = new ZLinkActivationConcurrencyAdmission(1);
            timerScheduler = new ZLinkTimerScheduler();
            catalog = new ZLinkSpotNodeCatalog(
                provider,
                runtime,
                runtime.Registration,
                nodeRuntime.Registration,
                nodeRuntime.Node,
                "objects",
                lifecycle: null,
                timerScheduler: timerScheduler,
                activationAdmission: admission
            );

            // The target received a Restore: one admission until target commit.
            var committed = await RestoreAsync(catalog, "restore-committed");
            Assert.Equal(1, admission.Active);
            var full = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await RestoreAsync(catalog, "restore-rejected")
            );
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, full.Kind);
            Assert.Equal(1, admission.Active);

            await catalog.PublishRelocatedReservedAsync(committed);
            Assert.Equal(0, admission.Active);

            // An aborted Restore ends its admission when the prepared target is cleaned up.
            var aborted = await RestoreAsync(catalog, "restore-aborted");
            Assert.Equal(1, admission.Active);
            await catalog.DiscardReservedAsync(aborted);
            Assert.Equal(0, admission.Active);
        }
        finally
        {
            if (catalog is not null)
                await catalog.DisposeAsync();
            if (timerScheduler is not null)
                await timerScheduler.DisposeAsync();
            await runtime.StopAsync(CancellationToken.None);
            await locations.RemoveOwnedRowsBeforeRoutingIdReleaseAsync(CancellationToken.None);
            await locations.StopAsync(CancellationToken.None);
        }
    }

    private static async Task<PreparedReservedSpot> RestoreAsync(
        ZLinkSpotNodeCatalog catalog,
        string prefix
    ) =>
        await catalog.PrepareInstanceReservedAsync(
            StableType,
            $"{prefix}-{Guid.NewGuid():N}",
            objectGeneration: 1,
            authorityOwnerGeneration: 1,
            CancellationToken.None,
            restoreLogicalTimers: true
        );

    private sealed class AdmissionRestoreInstanceSpot(IZLinkInstanceSpotContext context)
        : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context { get; } = context;
    }
}
