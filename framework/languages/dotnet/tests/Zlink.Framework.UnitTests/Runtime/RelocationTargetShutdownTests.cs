using System.Reflection;
using System.Runtime.ExceptionServices;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Fact]
    public async Task Force_stop_retires_location_peers_before_transport_disposal()
    {
        using var flowListener = Environment.GetEnvironmentVariable(
            "ZLINK_RELOCATION_TEST_FLOW_PATH"
        )
            is { } flowPath
            ? new TestHostMessageFlowListener($"{flowPath}.force-stop")
            : null;
        var trace = new RelocationBehaviorTrace();
        var store = new ZLinkInMemoryProviderLocationStore();
        var roots = new SynchronizedRelocationStore();
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            true
        );
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            true
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
        var sourceRid = source.LocalNodeRid;
        var repository = source.Runtime.Registration.Locations.ResolveStore()!;

        await source.Runtime.ForceStopAsync(CancellationToken.None);

        Assert.DoesNotContain(
            (await repository.ListMeshNodesAsync(RelocationBehaviorHost.MeshName, default)).Items,
            descriptor => descriptor.Rid == sourceRid
        );
    }

    // Host relocation flow 05 result table: a target whose Shutdown admission
    // seal is closed rejects the canonical PREPARE before any relay-ready reply.
    // That is an explicit Restore refusal (RelocationFailed), not a Store failure,
    // and the source workload is restored.
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Source_relocate_to_shutdown_sealed_target_blocks_with_relocation_failed(
        bool spot
    ) => await RelocateToShutdownSealedTargetAsync(spot, holdSourceRead: false);

    [Fact]
    public async Task Shutdown_sealed_target_refusal_survives_deadline_during_source_authority_read() =>
        await RelocateToShutdownSealedTargetAsync(spot: false, holdSourceRead: true);

    private async Task RelocateToShutdownSealedTargetAsync(bool spot, bool holdSourceRead)
    {
        using var flowListener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener($"{flowPath}.sealed-target.{spot}")
            : null;
        using var debugCapture = new SealStageDebugCapture();
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseJoinHandler.TrySetResult();
        trace.ReleaseTargetLifecycle.TrySetResult();
        trace.ReleaseSourceLeave.TrySetResult();
        var probe = new CanonicalRelocationTransportProbe(trace);
        probe.ReleasePrepareCall.TrySetResult();
        probe.ReleaseCutoverSend.TrySetResult();
        var store = new ZLinkInMemoryProviderLocationStore();
        var roots = new SynchronizedRelocationStore();
        var objectId = $"sealed-{Guid.NewGuid():N}";
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            spot,
            canonicalTransportProbe: probe,
            relocateSpots: spot
        );
        if (spot)
            _ = await source
                .Services.GetRequiredService<IZLinkSpotManager>()
                .GetOrCreate(objectId, RelocationBehaviorHost.SpotType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(ZLinkMessage.Empty)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async();
        else
            _ = await source
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(objectId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async();
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            spot,
            canonicalTransportProbe: probe,
            relocateSpots: spot
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
        target.Runtime.DrainAdmission.ClaimShutdown();
        var settlement = holdSourceRead
            ? ShutdownSettlementRepository.Install(
                source.Runtime.Registration.Locations,
                order: 0,
                holdReadAfter: probe.PrepareFailure.Task
            )
            : null;

        var result = await source
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = TimeSpan.FromSeconds(10),
                },
                CancellationToken.None
            );

        Assert.Equal(ZLinkFrameworkRelocationOutcome.Blocked, result.Outcome);
        Assert.Equal(ZLinkFrameworkRelocationReason.RelocationFailed, result.Reason);
        Assert.True(probe.PrepareCallStarted.Task.IsCompletedSuccessfully);
        Assert.True(probe.PrepareFailure.Task.IsCompletedSuccessfully);
        Assert.False(probe.ReadyReplyReceived.Task.IsCompleted);
        Assert.Equal(0, trace.TargetLifecycleAttemptCount);
        if (settlement is not null)
        {
            Assert.True(settlement.Blocked.Task.IsCompletedSuccessfully);
            Assert.False(settlement.SourceReadCanBeCanceled);
        }
    }

    // Common relocation §4.4 and Location runtime §10: the owner orders seal
    // against verification, including a successful CAS whose response is lost.
    [Theory]
    [InlineData(false, 0)]
    [InlineData(false, 1)]
    [InlineData(false, 2)]
    [InlineData(true, 0)]
    [InlineData(true, 1)]
    [InlineData(true, 2)]
    [InlineData(true, 3)]
    public async Task Target_shutdown_orders_seal_against_verified_cutover(bool spot, int order)
    {
        using var flowListener = Environment.GetEnvironmentVariable(
            "ZLINK_RELOCATION_TEST_FLOW_PATH"
        )
            is { } flowPath
            ? new TestHostMessageFlowListener($"{flowPath}.{spot}.{order}")
            : null;
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseJoinHandler.TrySetResult();
        if (order != 3)
            trace.ReleaseTargetLifecycle.TrySetResult();
        trace.ReleaseSourceLeave.TrySetResult();
        var probe = new CanonicalRelocationTransportProbe(trace);
        probe.ReleasePrepareCall.TrySetResult();
        var store = new ZLinkInMemoryProviderLocationStore();
        ShutdownSettlementRepository? settlement = null;
        var roots = new SynchronizedRelocationStore();
        var objectId = $"shutdown-{Guid.NewGuid():N}";
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            spot,
            canonicalTransportProbe: probe,
            relocateSpots: spot
        );
        if (spot)
            _ = await source
                .Services.GetRequiredService<IZLinkSpotManager>()
                .GetOrCreate(objectId, RelocationBehaviorHost.SpotType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(ZLinkMessage.Empty)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async();
        else
            _ = await source
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(objectId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async();
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            spot,
            canonicalTransportProbe: probe,
            relocateSpots: spot
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
        using var relocationCancellation = new CancellationTokenSource();
        var relocate = source
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = TimeSpan.FromSeconds(10),
                },
                relocationCancellation.Token
            )
            .AsTask();
        try
        {
            if (order == 3)
                await trace.WaitAsync("targetRestoreStarted");
            else
                await probe.CutoverSendStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            settlement = ShutdownSettlementRepository.Install(
                target.Runtime.Registration.Locations,
                order
            );
            Task? cutover = null;
            if (order is 1 or 2)
            {
                probe.ReleaseCutoverSend.TrySetResult();
                cutover = probe.CutoverSendSubmitted.Task;
                await settlement.Blocked.Task.WaitAsync(TimeSpan.FromSeconds(5));
            }
            var shutdown = target
                .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                .ShutdownAsync(TimeSpan.FromSeconds(3))
                .AsTask();
            await WaitUntilAsync(() => target.Runtime.DrainAdmission.IsSealedForShutdown);
            if (order == 3)
            {
                Assert.False(shutdown.IsCompleted);
                Assert.True(
                    target.Runtime.DrainAdmission.SnapshotActorAdmissions().ActiveCount > 0
                );
                trace.ReleaseTargetLifecycle.TrySetResult();
            }
            if (order is 0 or 3)
            {
                var stopped = await shutdown.WaitAsync(TimeSpan.FromSeconds(5));
                Assert.Equal(ZLinkFrameworkTerminationOutcome.Stopped, stopped.Outcome);
                Assert.Equal(
                    0,
                    target
                        .Services.GetRequiredService<ZLinkSpotRetireTargetRuntime>()
                        .ActiveStageCount
                );
                Assert.Empty(
                    target.Runtime.StandaloneActorRelocationRuntime.SnapshotPendingAttemptNames()
                );
                if (order == 0)
                    await probe.DeliverCapturedCutoverAsync();
                Assert.Equal(0, settlement.TargetCasCount);
            }
            else
            {
                Assert.False(shutdown.IsCompleted);
                if (spot)
                    Assert.Equal(
                        1,
                        target
                            .Services.GetRequiredService<ZLinkSpotRetireTargetRuntime>()
                            .ActiveStageCount
                    );
                else
                    Assert.Single(
                        target.Runtime.StandaloneActorRelocationRuntime.SnapshotPendingAttemptNames()
                    );
                if (spot && order == 1)
                {
                    var targetRid = target.LocalNodeRid;
                    await WaitUntilAsync(async () =>
                        (
                            await target
                                .Runtime.Registration.Locations.ResolveStore()!
                                .ListMeshNodesAsync(RelocationBehaviorHost.MeshName, default)
                        ).Items.Any(descriptor =>
                            descriptor.Rid == targetRid
                            && descriptor.State == ZLinkFrameworkRuntimeState.Draining
                        )
                    );
                }
                settlement.Release.TrySetResult();
                await cutover!.WaitAsync(TimeSpan.FromSeconds(5));
                await settlement.Settled.Task.WaitAsync(TimeSpan.FromSeconds(5));
                // Location runtime §10: a definitive conflict is also settlement.
                // The observed Draining descriptor precedes this delayed Spot CAS, so
                // the provider rejects its target placement. Response-loss tests
                // instead apply the CAS before seal and confirm it with a read.
                if (spot && order == 1)
                    Assert.Equal(ZLinkAggregateCommitResult.Stale, settlement.CommitResult);
                else
                    Assert.True(
                        settlement.CommitResult
                            is ZLinkAuthorityCompareExchangeResult.Stored
                                or ZLinkAggregateCommitResult.Committed
                                or ZLinkAggregateCommitResult.AlreadyCommitted,
                        $"Target CAS result: {settlement.CommitResult}"
                    );
                Assert.Equal(1, settlement.TargetCasCount);
                Assert.Equal(
                    ZLinkFrameworkTerminationOutcome.Stopped,
                    (await shutdown.WaitAsync(TimeSpan.FromSeconds(5))).Outcome
                );
            }
        }
        finally
        {
            settlement?.Release.TrySetResult();
            trace.ReleaseTargetLifecycle.TrySetResult();
            probe.ReleaseCutoverSend.TrySetResult();
            relocationCancellation.Cancel();
            // Cancel the source's held wire send through its existing shutdown
            // path; the target's local discard cannot authorize source resume.
            await source.Runtime.ForceStopAsync(CancellationToken.None);
            try
            {
                _ = await relocate;
            }
            catch (OperationCanceledException) when (relocationCancellation.IsCancellationRequested)
            { }
        }
    }
}

internal sealed class ShutdownBehaviorSpotAdapter(RelocationBehaviorTrace trace)
    : IZLinkSpotRelocationAdapter<BehaviorTargetSpot>
{
    public ValueTask<byte[]> CaptureAsync(BehaviorTargetSpot spot, CancellationToken token) =>
        ValueTask.FromResult(Array.Empty<byte>());

    public async ValueTask RestoreAsync(
        BehaviorTargetSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken token
    )
    {
        trace.Record("targetRestoreStarted");
        await trace.ReleaseTargetLifecycle.Task.WaitAsync(token);
    }
}

public class ShutdownSettlementRepository : DispatchProxy
{
    private IZLinkLocationRepository _inner = null!;
    private int _order;
    private bool _responseLost;
    private Task? _holdReadAfter;
    private bool _sourceReadHeld;
    internal bool SourceReadCanBeCanceled;
    internal int TargetCasCount;
    internal object? CommitResult;
    internal TaskCompletionSource Blocked { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    internal TaskCompletionSource Release { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    internal TaskCompletionSource Settled { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    internal static ShutdownSettlementRepository Install(
        ZLinkLocationRegistration registration,
        int order,
        Task? holdReadAfter = null
    )
    {
        var repository = Create<IZLinkLocationRepository, ShutdownSettlementRepository>();
        var proxy = (ShutdownSettlementRepository)repository;
        proxy._inner = registration.ResolveStore()!;
        proxy._order = order;
        proxy._holdReadAfter = holdReadAfter;
        typeof(ZLinkLocationRegistration)
            .GetField("_repository", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(registration, repository);
        return proxy;
    }

    protected override object? Invoke(MethodInfo? method, object?[]? args)
    {
        if (
            method!.Name == nameof(IZLinkLocationRepository.CompareExchangeAuthorityAsync)
            && args![2]
                is ZLinkAuthorityMutation.Put
                {
                    GenerationTransition: ZLinkAuthorityGenerationTransition.NewOwner
                }
        )
            return CommitAsync(
                () =>
                    _inner.CompareExchangeAuthorityAsync(
                        (ZLinkAuthorityKey)args![0]!,
                        (string)args[1]!,
                        (ZLinkAuthorityMutation)args[2]!,
                        (CancellationToken)args[3]!
                    ),
                (CancellationToken)args![3]!
            );
        if (method.Name == nameof(IZLinkLocationRepository.CommitAggregateAsync))
            return CommitAsync(
                () =>
                    _inner.CommitAggregateAsync(
                        (ZLinkAggregateFence)args![0]!,
                        (CancellationToken)args[1]!
                    ),
                (CancellationToken)args![1]!
            );
        if (method.Name == nameof(IZLinkLocationRepository.ReadAuthorityAsync))
            return ReadAsync((ZLinkAuthorityKey)args![0]!, (CancellationToken)args[1]!);
        try
        {
            return method.Invoke(_inner, args);
        }
        catch (TargetInvocationException exception) when (exception.InnerException is not null)
        {
            ExceptionDispatchInfo.Throw(exception.InnerException);
            throw;
        }
    }

    private async ValueTask<T> CommitAsync<T>(Func<ValueTask<T>> commit, CancellationToken token)
    {
        Interlocked.Increment(ref TargetCasCount);
        if (_order == 1)
        {
            Blocked.TrySetResult();
            await Release.Task.WaitAsync(token);
        }
        var result = await commit();
        CommitResult = result;
        if (_order == 2)
        {
            _responseLost = true;
            throw new IOException("Injected loss of the applied target CAS response.");
        }
        Settled.TrySetResult();
        return result;
    }

    private async ValueTask<ZLinkAuthorityReadResult> ReadAsync(
        ZLinkAuthorityKey key,
        CancellationToken token
    )
    {
        if (!_sourceReadHeld && _holdReadAfter is { IsCompletedSuccessfully: true })
        {
            _sourceReadHeld = true;
            SourceReadCanBeCanceled = token.CanBeCanceled;
            Blocked.TrySetResult();
            if (token.CanBeCanceled)
                await Task.Delay(Timeout.InfiniteTimeSpan, token);
        }
        if (_responseLost)
        {
            Blocked.TrySetResult();
            await Release.Task.WaitAsync(token);
        }
        var result = await _inner.ReadAuthorityAsync(key, token);
        if (_responseLost)
        {
            Assert.IsType<ZLinkAuthorityReadResult.Found>(result);
            Settled.TrySetResult();
        }
        return result;
    }
}
