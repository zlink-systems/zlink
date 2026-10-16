using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
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

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task Relocated_initialization_can_register_timer_while_ingress_is_sealed(
        bool restoreSourceTimer,
        bool mismatchedHandler
    )
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options
                .AddRouteMesh("objects")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server()
                .AddSpotFactory<InitializingTimerSpot>(
                    "initializing-timer",
                    f => f.DisableRelocation()
                );
        });
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var locations = provider.GetRequiredService<ZLinkLocationRuntime>();
        await locations.StartAsync(runtime.PrepareLocationNodeRoutingId(), CancellationToken.None);
        try
        {
            await runtime.StartAsync(CancellationToken.None);
            var catalog = runtime.GetSpotNodeRuntime("objects").Catalog;
            var prepared = await catalog.PrepareReservedAsync(
                typeof(InitializingTimerSpot),
                "restored-timer",
                1,
                1,
                ZLinkMessage.Empty,
                CancellationToken.None,
                invokeCreate: false
            );
            try
            {
                var activation = Assert.IsType<ZLinkUserSpotActivation>(prepared.Activation);
                var seal = await activation.TrySealRelocationAsync();
                Assert.NotNull(seal);
                var startedAt = DateTimeOffset.FromUnixTimeSeconds(1_700_000_000);
                var period = TimeSpan.FromHours(2);
                var expected = new ZLinkTimerLogicalSnapshot(
                    "pulse",
                    period,
                    new ZLinkTimerOptions(),
                    startedAt,
                    5,
                    7,
                    startedAt + period * 10,
                    new ZLinkTimerTick(
                        "pulse",
                        6,
                        9,
                        period,
                        startedAt + period * 9,
                        startedAt + period * 9,
                        period * 9,
                        period * 9,
                        TimeSpan.Zero,
                        1
                    )
                );
                if (restoreSourceTimer)
                    await activation.RestoreLogicalTimersAsync([
                        ZLinkSpotTimerRelocationCodec.Encode(
                            new ZLinkSpotLogicalTimerSnapshot(
                                mismatchedHandler
                                    ? typeof(MismatchedInitializingTimerHandler)
                                    : typeof(InitializingTimerHandler),
                                typeof(InitializingTimerSpot),
                                expected
                            )
                        ),
                    ]);
                else
                    await activation.RestoreLogicalTimersAsync([]);
                if (mismatchedHandler)
                {
                    var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                        await activation.InitializeRelocatedUserSpotAsync(
                            seal,
                            CancellationToken.None
                        )
                    );
                    Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, failure.Kind);
                    Assert.Contains("handler type", failure.Message, StringComparison.Ordinal);
                    Assert.False(((InitializingTimerSpot)activation.Spot).Initialized);
                    return;
                }
                await activation.InitializeRelocatedUserSpotAsync(seal, CancellationToken.None);
                var spot = (InitializingTimerSpot)activation.Spot;
                Assert.True(spot.Initialized);
                var timer = Assert.IsType<ZLinkTimer>(spot.Timer);
                if (restoreSourceTimer)
                    Assert.Equal(expected, timer.Snapshot());
                await Assert.ThrowsAsync<ZLinkConfigurationException>(async () =>
                    await spot.Context.AddTimer<InitializingTimerHandler>(
                        "pulse",
                        TimeSpan.FromHours(1)
                    )
                );
            }
            finally
            {
                await catalog.DiscardReservedAsync(prepared);
            }
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
            await locations.RemoveOwnedRowsBeforeRoutingIdReleaseAsync(CancellationToken.None);
            await locations.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Relocation_target_configuration_reattaches_scanned_timer()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options
                .AddRouteMesh("scanned-timers")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server()
                .AddSpotFactory<ScannedTimerSpot>(
                    "scanned-timer",
                    factory => factory.DisableRelocation()
                );
        });
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var locations = provider.GetRequiredService<ZLinkLocationRuntime>();
        await locations.StartAsync(runtime.PrepareLocationNodeRoutingId(), CancellationToken.None);
        try
        {
            await runtime.StartAsync(CancellationToken.None);
            var catalog = runtime.GetSpotNodeRuntime("scanned-timers").Catalog;
            var source = await catalog.PrepareReservedAsync(
                typeof(ScannedTimerSpot),
                "scanned-source",
                1,
                1,
                ZLinkMessage.Empty,
                CancellationToken.None
            );
            try
            {
                var sourceSeal = await source.Activation.TrySealRelocationAsync();
                Assert.NotNull(sourceSeal);
                Assert.Single(sourceSeal.LogicalTimers);
                var target = await catalog.PrepareReservedAsync(
                    typeof(ScannedTimerSpot),
                    "scanned-target",
                    1,
                    1,
                    ZLinkMessage.Empty,
                    CancellationToken.None,
                    invokeCreate: false,
                    logicalTimers: sourceSeal.LogicalTimers
                );
                try
                {
                    var targetSeal = await target.Activation.TrySealRelocationAsync();
                    Assert.NotNull(targetSeal);
                    var restored = Assert.Single(targetSeal.LogicalTimers);
                    Assert.Equal(
                        ZLinkSpotTimerRelocationCodec.Decode(sourceSeal.LogicalTimers[0]),
                        ZLinkSpotTimerRelocationCodec.Decode(restored)
                    );
                }
                finally
                {
                    await catalog.DiscardReservedAsync(target);
                }
            }
            finally
            {
                await catalog.DiscardReservedAsync(source);
            }
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
            await locations.RemoveOwnedRowsBeforeRoutingIdReleaseAsync(CancellationToken.None);
            await locations.StopAsync(CancellationToken.None);
        }
    }

    private sealed class ScannedTimerSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        public void Configure() { }
    }

    [ZLinkSpotTimerHandler("auto-pulse", 3_600_000)]
    private sealed class ScannedTimerHandler : IZLinkSpotTimerHandler<ScannedTimerSpot>
    {
        public ValueTask HandleAsync(
            ScannedTimerSpot spot,
            ZLinkTimerTick tick,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    private sealed class InitializingTimerSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
        public bool Initialized { get; private set; }
        public IZLinkTimer? Timer { get; private set; }

        public void Configure() { }

        public async ValueTask OnInitializeAsync(CancellationToken cancellationToken)
        {
            Timer = await Context.AddTimer<InitializingTimerHandler>(
                "pulse",
                TimeSpan.FromHours(1),
                cancellationToken: cancellationToken
            );
            Initialized = true;
        }
    }

    private sealed class MismatchedInitializingTimerHandler
        : IZLinkSpotTimerHandler<InitializingTimerSpot>
    {
        public ValueTask HandleAsync(
            InitializingTimerSpot spot,
            ZLinkTimerTick tick,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    private sealed class InitializingTimerHandler : IZLinkSpotTimerHandler<InitializingTimerSpot>
    {
        public ValueTask HandleAsync(
            InitializingTimerSpot spot,
            ZLinkTimerTick tick,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

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
