using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Configuration.Builders;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

// Spot base/delta relocation capture (spec 15 §5): mirrors the Actor-side
// matrix in StandaloneActorRelocationRuntimeTests.cs's "Base/delta
// relocation" region.
public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public void PreserveStateWith_selects_the_base_delta_invoker_for_a_capable_UserSpot_adapter()
    {
        var builder = new ZLinkUserSpotFactoryBuilder<TestBaseDeltaSpot>();
        builder.PreserveStateWith<BaseDeltaCapableSpotAdapter>();

        Assert.IsType<ZLinkSpotBaseDeltaRelocationAdapterInvoker<TestBaseDeltaSpot>>(
            builder.Relocation.AdapterInvoker);
    }

    [Fact]
    public void PreserveStateWith_keeps_the_plain_invoker_for_a_non_capable_UserSpot_adapter()
    {
        var builder = new ZLinkUserSpotFactoryBuilder<TestBaseDeltaSpot>();
        builder.PreserveStateWith<PlainSpotRelocationAdapter>();

        Assert.IsType<ZLinkSpotRelocationAdapterInvoker<TestBaseDeltaSpot>>(
            builder.Relocation.AdapterInvoker);
    }

    [Fact]
    public void IsBaseDeltaCapable_reflects_the_registered_Spot_adapter()
    {
        Assert.True(ZLinkActorRelocationRegistry.IsBaseDeltaCapable(
            CreateSpotRelocation<BaseDeltaCapableSpotAdapter>(baseDeltaCapable: true)));
        Assert.False(ZLinkActorRelocationRegistry.IsBaseDeltaCapable(
            CreateSpotRelocation<PlainSpotRelocationAdapter>(baseDeltaCapable: false)));
    }

    [Fact]
    public async Task Base_delta_capture_and_restore_round_trip_through_the_Spot_invoker()
    {
        //  Both-stages e2e through the exact invoker class the production
        //  wiring resolves (ZLinkUserSpotFactoryBuilder.PreserveStateWith
        //  selects it; ZLinkSpotRetireScheduler / ZLinkSpotActivationExecution
        //  call through it via the same IZLinkBaseDeltaRelocationAdapterInvoker
        //  contract) — with stage-distinguishing byte markers so a bug that
        //  swapped base/delta or skipped a stage would be caught.
        var adapter = new BaseDeltaCapableSpotAdapter();
        var services = new ServiceCollection()
            .AddSingleton(adapter)
            .BuildServiceProvider();
        IZLinkBaseDeltaRelocationAdapterInvoker invoker =
            new ZLinkSpotBaseDeltaRelocationAdapterInvoker<TestBaseDeltaSpot>(
                typeof(BaseDeltaCapableSpotAdapter));
        var spot = new TestBaseDeltaSpot(context: null!);

        var baseBytes = await invoker.CaptureBaseAsync(
            services, spot, CancellationToken.None);
        var deltaBytes = await invoker.CaptureDeltaAsync(
            services, spot, CancellationToken.None);
        await invoker.RestoreBaseAsync(
            services, spot, baseBytes, CancellationToken.None);
        await invoker.ApplyDeltaAsync(
            services, spot, deltaBytes, CancellationToken.None);

        Assert.Equal(
            ["CaptureBase", "CaptureDelta", "RestoreBase:3", "ApplyDelta:2"],
            adapter.Calls);
    }

    [Fact]
    public async Task RestorePreparedSpotStateAsync_BaseDeltaApplyFailure_RetriesOnAFreshActivation()
    {
        var adapter = new RetryOnceApplyDeltaSpotAdapter();
        var firstActivation = CreateSpotActivation(
            "base-delta-restore-fail",
            typeof(RetryOnceApplyDeltaSpotAdapter),
            new ZLinkSpotBaseDeltaRelocationAdapterInvoker<TestBaseDeltaSpot>(
                typeof(RetryOnceApplyDeltaSpotAdapter)),
            adapter,
            out var firstSpot);
        firstActivation.AttachSpot(firstSpot);
        var secondActivation = CreateSpotActivation(
            "base-delta-restore-fail",
            typeof(RetryOnceApplyDeltaSpotAdapter),
            new ZLinkSpotBaseDeltaRelocationAdapterInvoker<TestBaseDeltaSpot>(
                typeof(RetryOnceApplyDeltaSpotAdapter)),
            adapter,
            out var secondSpot);
        secondActivation.AttachSpot(secondSpot);
        var freshPrepareCalls = 0;
        var discarded = false;
        try
        {
            var restored = await ZLinkFrameworkRuntime
                .RestorePreparedSpotStateAsync(
                    new PreparedReservedSpot(firstActivation, false, null),
                    _ =>
                    {
                        freshPrepareCalls++;
                        return ValueTask.FromResult(new PreparedReservedSpot(
                            secondActivation, false, null));
                    },
                    async prepared =>
                    {
                        discarded = true;
                        await prepared.Activation.DisposeAsync();
                    },
                    _ => { },
                    state: new byte[] { 4, 5 },
                    basePayload: new byte[] { 1, 2, 3 },
                    hasBase: true,
                    CancellationToken.None);

            Assert.Same(secondActivation, restored.Activation);
            Assert.NotSame(firstSpot, secondSpot);
            Assert.True(discarded);
            Assert.Equal(1, freshPrepareCalls);
            Assert.Equal(["RestoreBase:3", "ApplyDelta:2", "RestoreBase:3", "ApplyDelta:2"], adapter.Calls);
            Assert.Equal(2, adapter.RestoredInstances.Count);
            Assert.NotSame(adapter.RestoredInstances[0], adapter.RestoredInstances[1]);
        }
        finally
        {
            await secondActivation.DisposeAsync();
        }
    }

    [Fact]
    public async Task RestorePreparedSpotStateAsync_SecondBaseDeltaFailure_IsExplicitInternalFailure()
    {
        var adapter = new FailingApplyDeltaSpotAdapter();
        var firstActivation = CreateAttachedActivation(adapter, out var firstSpot);
        var secondActivation = CreateAttachedActivation(adapter, out var secondSpot);
        var discarded = 0;
        try
        {
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(
                () => ZLinkFrameworkRuntime.RestorePreparedSpotStateAsync(
                        new PreparedReservedSpot(firstActivation, false, null),
                        _ => ValueTask.FromResult(new PreparedReservedSpot(
                            secondActivation, false, null)),
                        async prepared =>
                        {
                            discarded++;
                            await prepared.Activation.DisposeAsync();
                        },
                        _ => { },
                        state: new byte[] { 4, 5 },
                        basePayload: new byte[] { 1, 2, 3 },
                        hasBase: true,
                        CancellationToken.None)
                    .AsTask());

            Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, failure.Kind);
            Assert.Contains("apply failed", failure.Message);
            Assert.NotSame(firstSpot, secondSpot);
            Assert.Equal(
                ["RestoreBase:3", "ApplyDelta:2", "RestoreBase:3", "ApplyDelta:2"],
                adapter.Calls);
            Assert.Equal(1, discarded);
        }
        finally
        {
            await secondActivation.DisposeAsync();
        }
    }

    [Fact]
    public async Task RestoreSpotRelocationStateAsync_LegacyPath_IsByteIdenticalToTheSingleCaptureRestore()
    {
        //  Legacy-path regression: a non-base/delta-capable adapter keeps
        //  the single Capture/Restore path unaffected — no chunk-stage
        //  split, no base payload, and the plain RestoreAsync is what
        //  runs.
        var adapter = new RecordingPlainSpotRelocationAdapter();
        var activation = CreateSpotActivation(
            "legacy-restore",
            typeof(RecordingPlainSpotRelocationAdapter),
            new ZLinkSpotRelocationAdapterInvoker<TestBaseDeltaSpot>(
                typeof(RecordingPlainSpotRelocationAdapter)),
            adapter,
            out var spot);
        activation.AttachSpot(spot);
        await using var cleanup = activation;

        var freshPrepareCalls = 0;
        var restored = await ZLinkFrameworkRuntime.RestorePreparedSpotStateAsync(
            new PreparedReservedSpot(activation, false, null),
            _ =>
            {
                freshPrepareCalls++;
                throw new InvalidOperationException("legacy restore must not retry");
            },
            _ => throw new InvalidOperationException("legacy restore must not discard"),
            _ => throw new InvalidOperationException("legacy restore must not replace"),
            state: new byte[] { 9, 9 },
            basePayload: default,
            hasBase: false,
            CancellationToken.None);

        Assert.Same(activation, restored.Activation);
        Assert.Equal(0, freshPrepareCalls);
        Assert.Equal(new byte[] { 9, 9 }, adapter.RestoredPayload);
    }

    private static ZLinkUserSpotActivation CreateAttachedActivation(
        FailingApplyDeltaSpotAdapter adapter,
        out TestBaseDeltaSpot spot)
    {
        var activation = CreateSpotActivation(
            "base-delta-restore-fail",
            typeof(FailingApplyDeltaSpotAdapter),
            new ZLinkSpotBaseDeltaRelocationAdapterInvoker<TestBaseDeltaSpot>(
                typeof(FailingApplyDeltaSpotAdapter)),
            adapter,
            out spot);
        activation.AttachSpot(spot);
        return activation;
    }

    private static ZLinkUserSpotActivation CreateSpotActivation(
        string spotId,
        Type adapterType,
        IZLinkRelocationAdapterInvoker invoker,
        object adapterInstance,
        out TestBaseDeltaSpot spot)
    {
        var services = new ServiceCollection()
            .AddSingleton(adapterInstance.GetType(), adapterInstance)
            .BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration();
        registration.SpotNodes["node"] = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "node"
        };
        registration.SpotNodes["node"].SpotRelocations["base-delta-spot"] =
            new ZLinkObjectRelocationRegistration(
                typeof(TestBaseDeltaSpot),
                new ZLinkObjectPlacementOptions(),
                2,
                adapterType,
                invoker);
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new ThrowingBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            new CapturingSpot(),
            spotId,
            RoutingId.From("node"),
            "node",
            "channel",
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(1));
        spot = new TestBaseDeltaSpot(activation);
        return activation;
    }

    private sealed class TestBaseDeltaSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
    }

    private sealed class BaseDeltaCapableSpotAdapter
        : IZLinkSpotBaseDeltaRelocationAdapter<TestBaseDeltaSpot>
    {
        internal List<string> Calls { get; } = [];

        public ValueTask<byte[]> CaptureAsync(
            TestBaseDeltaSpot spot, CancellationToken cancellationToken)
        {
            Calls.Add("Capture");
            return ValueTask.FromResult<byte[]>([9]);
        }

        public ValueTask RestoreAsync(
            TestBaseDeltaSpot spot,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
        {
            Calls.Add("Restore:" + payload.Length);
            return ValueTask.CompletedTask;
        }

        public ValueTask<byte[]> CaptureBaseAsync(
            TestBaseDeltaSpot spot, CancellationToken cancellationToken)
        {
            Calls.Add("CaptureBase");
            return ValueTask.FromResult<byte[]>([1, 2, 3]);
        }

        public ValueTask<byte[]> CaptureDeltaAsync(
            TestBaseDeltaSpot spot, CancellationToken cancellationToken)
        {
            Calls.Add("CaptureDelta");
            return ValueTask.FromResult<byte[]>([4, 5]);
        }

        public ValueTask RestoreBaseAsync(
            TestBaseDeltaSpot spot,
            ReadOnlyMemory<byte> basePayload,
            CancellationToken cancellationToken)
        {
            Calls.Add("RestoreBase:" + basePayload.Length);
            return ValueTask.CompletedTask;
        }

        public ValueTask ApplyDeltaAsync(
            TestBaseDeltaSpot spot,
            ReadOnlyMemory<byte> deltaPayload,
            CancellationToken cancellationToken)
        {
            Calls.Add("ApplyDelta:" + deltaPayload.Length);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class FailingApplyDeltaSpotAdapter
        : IZLinkSpotBaseDeltaRelocationAdapter<TestBaseDeltaSpot>
    {
        internal List<string> Calls { get; } = [];

        public ValueTask<byte[]> CaptureAsync(
            TestBaseDeltaSpot spot, CancellationToken cancellationToken) =>
            ValueTask.FromResult(Array.Empty<byte>());

        public ValueTask RestoreAsync(
            TestBaseDeltaSpot spot,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask<byte[]> CaptureBaseAsync(
            TestBaseDeltaSpot spot, CancellationToken cancellationToken) =>
            ValueTask.FromResult(Array.Empty<byte>());

        public ValueTask<byte[]> CaptureDeltaAsync(
            TestBaseDeltaSpot spot, CancellationToken cancellationToken) =>
            ValueTask.FromResult(Array.Empty<byte>());

        public ValueTask RestoreBaseAsync(
            TestBaseDeltaSpot spot,
            ReadOnlyMemory<byte> basePayload,
            CancellationToken cancellationToken)
        {
            Calls.Add("RestoreBase:" + basePayload.Length);
            return ValueTask.CompletedTask;
        }

        public ValueTask ApplyDeltaAsync(
            TestBaseDeltaSpot spot,
            ReadOnlyMemory<byte> deltaPayload,
            CancellationToken cancellationToken)
        {
            Calls.Add("ApplyDelta:" + deltaPayload.Length);
            throw new InvalidOperationException("apply-delta boom");
        }
    }

    private sealed class RetryOnceApplyDeltaSpotAdapter
        : IZLinkSpotBaseDeltaRelocationAdapter<TestBaseDeltaSpot>
    {
        private int _applyAttempts;

        internal List<string> Calls { get; } = [];

        internal List<TestBaseDeltaSpot> RestoredInstances { get; } = [];

        public ValueTask<byte[]> CaptureAsync(
            TestBaseDeltaSpot spot, CancellationToken cancellationToken) =>
            ValueTask.FromResult(Array.Empty<byte>());

        public ValueTask RestoreAsync(
            TestBaseDeltaSpot spot,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask<byte[]> CaptureBaseAsync(
            TestBaseDeltaSpot spot, CancellationToken cancellationToken) =>
            ValueTask.FromResult(Array.Empty<byte>());

        public ValueTask<byte[]> CaptureDeltaAsync(
            TestBaseDeltaSpot spot, CancellationToken cancellationToken) =>
            ValueTask.FromResult(Array.Empty<byte>());

        public ValueTask RestoreBaseAsync(
            TestBaseDeltaSpot spot,
            ReadOnlyMemory<byte> basePayload,
            CancellationToken cancellationToken)
        {
            Calls.Add("RestoreBase:" + basePayload.Length);
            RestoredInstances.Add(spot);
            return ValueTask.CompletedTask;
        }

        public ValueTask ApplyDeltaAsync(
            TestBaseDeltaSpot spot,
            ReadOnlyMemory<byte> deltaPayload,
            CancellationToken cancellationToken)
        {
            Calls.Add("ApplyDelta:" + deltaPayload.Length);
            if (Interlocked.Increment(ref _applyAttempts) == 1)
                throw new InvalidOperationException("apply-delta boom");
            return ValueTask.CompletedTask;
        }
    }

    private sealed class PlainSpotRelocationAdapter
        : IZLinkSpotRelocationAdapter<TestBaseDeltaSpot>
    {
        public ValueTask<byte[]> CaptureAsync(
            TestBaseDeltaSpot spot, CancellationToken cancellationToken) =>
            ValueTask.FromResult(Array.Empty<byte>());

        public ValueTask RestoreAsync(
            TestBaseDeltaSpot spot,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }

    private sealed class RecordingPlainSpotRelocationAdapter
        : IZLinkSpotRelocationAdapter<TestBaseDeltaSpot>
    {
        internal byte[]? RestoredPayload { get; private set; }

        public ValueTask<byte[]> CaptureAsync(
            TestBaseDeltaSpot spot, CancellationToken cancellationToken) =>
            ValueTask.FromResult<byte[]>([1, 2, 3]);

        public ValueTask RestoreAsync(
            TestBaseDeltaSpot spot,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
        {
            RestoredPayload = payload.ToArray();
            return ValueTask.CompletedTask;
        }
    }

    private static ZLinkObjectRelocationRegistration CreateSpotRelocation<TAdapter>(
        bool baseDeltaCapable)
        where TAdapter : class, IZLinkSpotRelocationAdapter<TestBaseDeltaSpot> =>
        new(
            typeof(TestBaseDeltaSpot),
            new ZLinkObjectPlacementOptions(),
            2,
            typeof(TAdapter),
            baseDeltaCapable
                ? new ZLinkSpotBaseDeltaRelocationAdapterInvoker<TestBaseDeltaSpot>(
                    typeof(TAdapter))
                : new ZLinkSpotRelocationAdapterInvoker<TestBaseDeltaSpot>(
                    typeof(TAdapter)));
}
