using System.Reflection;
using System.Runtime.ExceptionServices;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Fact]
    public Task SpotWide_find_follows_target_dispatch_before_steady_normalization() =>
        RunSpotWideFindReadinessAsync(verifyClose: false);

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public Task SpotWide_close_reports_retryable_moving_before_steady_normalization(
        bool retainSourceActivation
    ) => RunSpotWideFindReadinessAsync(verifyClose: true, retainSourceActivation);

    private static async Task RunSpotWideFindReadinessAsync(
        bool verifyClose,
        bool retainSourceActivation = false
    )
    {
        using var listener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(
                verifyClose
                    ? flowPath + (retainSourceActivation ? ".source-retained" : ".source-detached")
                    : flowPath
            )
            : null;
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseTargetLifecycle.TrySetResult();
        var store = new ZLinkInMemoryProviderLocationStore();
        var roots = new SynchronizedRelocationStore();
        var spotId = $"find-ready-{Guid.NewGuid():N}";
        var timeout = TimeSpan.FromSeconds(10);
        var sourceClosing = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var sourceTrace = new RelocationBehaviorTrace
        {
            OnTargetClosing = () =>
            {
                sourceClosing.TrySetResult();
                return ValueTask.CompletedTask;
            },
        };
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            sourceTrace,
            store,
            roots,
            true,
            relocateSpots: true
        );
        var sourceSpots = source.Services.GetRequiredService<IZLinkSpotManager>();
        _ = await sourceSpots
            .GetOrCreate(spotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Timeout(timeout)
            .Async();
        var original = Assert.IsType<SpotRef>(await sourceSpots.FindAsync(spotId));

        var initializing = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseInitialization = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        trace.InitializeTarget = async (_, cancellationToken) =>
        {
            initializing.TrySetResult();
            await releaseInitialization.Task.WaitAsync(cancellationToken);
        };
        var closeSubmissions = new List<(RoutingId Destination, UserSpotCloseFence Fence)>();
        void RecordClose(RoutingId destination, UserSpotCloseFence fence) =>
            closeSubmissions.Add((destination, fence));

        var closeProbe = new CanonicalRelocationTransportProbe(trace)
        {
            OnUserSpotClose = RecordClose,
        };
        var closingCalls = 0;
        trace.OnTargetClosing = () =>
        {
            Interlocked.Increment(ref closingCalls);
            return ValueTask.CompletedTask;
        };
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            true,
            canonicalTransportProbe: verifyClose ? closeProbe : null,
            relocateSpots: true
        );
        var locations = target.Runtime.Registration.Locations;
        var normalization = SpotWideNormalizationGate.Install(locations);
        var authorityStore = new ZLinkProviderLocationRepository(store);
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
        var sourceCatalog = source
            .Runtime.GetSpotNodeRuntime(RelocationBehaviorHost.MeshName)
            .Catalog;
        var sourceObservation = verifyClose
            ? SpotWideNormalizationGate.InstallSourceReadGate(
                source.Runtime.Registration.Locations,
                key,
                original.ObjectGeneration,
                target.LocalNodeRid
            )
            : null;
        bool SourceHostsOriginal() =>
            sourceCatalog.Spots.Any(activation =>
                activation.SpotId == spotId
                && activation.ObjectGeneration == original.ObjectGeneration
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

        await using var observer = verifyClose
            ? await RelocationBehaviorHost.StartAsync(
                "observer",
                sourceTrace,
                store,
                roots,
                registerTargetSpot: false,
                canonicalTransportProbe: new CanonicalRelocationTransportProbe(sourceTrace)
                {
                    OnUserSpotClose = RecordClose,
                }
            )
            : null;
        if (observer is not null)
            await WaitUntilAsync(() =>
                observer
                    .Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                    .Node.Status()
                    .ActivePeerCount == 2
                && target
                    .Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                    .Node.Status()
                    .ActivePeerCount == 2
            );

        using var cancellation = new CancellationTokenSource();
        var relocation = source
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = TimeSpan.FromSeconds(30),
                },
                cancellation.Token
            )
            .AsTask();
        try
        {
            await initializing.Task.WaitAsync(timeout);
            var committed = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(
                    await authorityStore.ReadAuthorityAsync(key)
                )
                .Snapshot;
            Assert.Equal(target.LocalNodeRid, committed.Allocation.Descriptor.Rid);
            Assert.Equal(original.ObjectGeneration, committed.ObjectGeneration);
            var spots = target.Services.GetRequiredService<IZLinkSpotManager>();
            var beforeDispatch = await spots.FindAsync(spotId);

            if (sourceObservation is not null)
            {
                await sourceObservation.BeforeLocalFence.Task.WaitAsync(timeout);
                Assert.True(SourceHostsOriginal());
            }

            normalization.NormalizationAggregateId = committed.Relocation!.RelocationId;
            releaseInitialization.TrySetResult();
            await normalization.Entered.Task.WaitAsync(timeout);
            var opened = Assert.IsType<SpotRef>(await spots.FindAsync(spotId));
            Assert.Equal(original with { NodeRid = target.LocalNodeRid }, opened);
            Assert.Null(beforeDispatch);
            var activated = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(
                    await authorityStore.ReadAuthorityAsync(key)
                )
                .Snapshot;
            Assert.Equal(
                committed.Relocation! with
                {
                    Phase = ServiceWireCodec.RelocationPhase.Activated,
                },
                activated.Relocation
            );
            Assert.Equal(committed.AuthorityOwnerGeneration, activated.AuthorityOwnerGeneration);

            if (sourceObservation is not null && observer is not null)
            {
                Assert.True(
                    ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                        activated.Payload.Span,
                        out var raw
                    )
                );
                // Location runtime §8.2·§8.3: the owner-changing commit projects the target
                // route recorded on the aggregate into the authority payload; the sealed
                // source values stay in the aggregate inventory record.
                Assert.Equal(target.LocalNodeRid, raw.NodeRid);
                if (!retainSourceActivation)
                {
                    sourceObservation.ReleaseLocalFence.TrySetResult();
                    // The source invokes its relocation closing callback after catalog detach.
                    await sourceClosing.Task.WaitAsync(timeout);
                }
                Assert.Equal(retainSourceActivation, SourceHostsOriginal());

                var closeError = await Record.ExceptionAsync(() =>
                    spots.CloseAsync(opened).AsTask()
                );
                Assert.True(
                    closeSubmissions.Count == 0,
                    $"Local Close submitted to [{string.Join(", ", closeSubmissions.Select(call => call.Destination))}]. Prior source: {original.NodeRid}. Terminal: {closeError?.Message}"
                );
                AssertSpotWideCloseMoving(closeError);

                var remoteSpots = observer.Services.GetRequiredService<IZLinkSpotManager>();
                var remoteError = await Record.ExceptionAsync(() =>
                    remoteSpots.CloseAsync(original).AsTask()
                );
                var submission = Assert.Single(closeSubmissions);
                Assert.Equal(target.LocalNodeRid, submission.Destination);
                Assert.Equal(
                    new UserSpotCloseFence(
                        spotId,
                        opened.ObjectGeneration,
                        target.LocalNodeRid,
                        activated.Allocation.DescriptorLifecycleGeneration,
                        activated.AuthorityOwnerGeneration,
                        activated.StoreVersion
                    ),
                    submission.Fence
                );
                AssertSpotWideCloseMoving(remoteError);

                var afterClose = Assert
                    .IsType<ZLinkAuthorityReadResult.Found>(
                        await authorityStore.ReadAuthorityAsync(key)
                    )
                    .Snapshot;
                Assert.Equal(activated.StoreVersion, afterClose.StoreVersion);
                Assert.Equal(activated.Relocation, afterClose.Relocation);
                Assert.Equal(opened.ObjectGeneration, afterClose.ObjectGeneration);
                Assert.Equal(0, Volatile.Read(ref closingCalls));

                var stale = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                    spots
                        .CloseAsync(
                            opened with
                            {
                                ObjectGeneration = opened.ObjectGeneration == 1 ? 2UL : 1UL,
                            }
                        )
                        .AsTask()
                );
                Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, stale.Kind);
                Assert.Equal(
                    (int)ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale,
                    stale.FrameworkFailureCode
                );
                sourceObservation.ReleaseLocalFence.TrySetResult();
            }

            normalization.Release.TrySetResult();
            Assert.Equal(
                ZLinkAggregateCommitResult.Committed,
                await normalization.Settled.Task.WaitAsync(timeout)
            );
            Assert.Equal(opened, await spots.FindAsync(spotId));
            Assert.Equal(
                ZLinkFrameworkRelocationOutcome.Relocated,
                (await relocation.WaitAsync(timeout)).Outcome
            );
            if (verifyClose)
            {
                Assert.True(await spots.CloseAsync(opened));
                Assert.False(await spots.CloseAsync(opened));
                Assert.Equal(1, Volatile.Read(ref closingCalls));
            }
        }
        finally
        {
            releaseInitialization.TrySetResult();
            sourceObservation?.ReleaseLocalFence.TrySetResult();
            normalization.Release.TrySetResult();
            cancellation.Cancel();
            await ((Task)relocation).ConfigureAwait(
                ConfigureAwaitOptions.SuppressThrowing
                    | ConfigureAwaitOptions.ContinueOnCapturedContext
            );
        }
    }

    private static void AssertSpotWideCloseMoving(Exception? error)
    {
        var moving = Assert.IsType<ZLinkFrameworkException>(error);
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, moving.Kind);
        Assert.Equal(ZLinkRetryAdvice.RetryAfterBackoff, moving.RetryAdvice);
        Assert.Equal(
            (int)ServiceWireConstants.FrameworkErrorCode.SpotMoving,
            moving.FrameworkFailureCode
        );
    }
}

public class SpotWideNormalizationGate : DispatchProxy
{
    private IZLinkLocationRepository _store = null!;
    private ZLinkAuthorityKey? _spotBeforeLocalFence;
    private (ulong ObjectGeneration, RoutingId TargetRid)? _sourceReadTarget;
    private int _sourceReadClaimed;

    internal Guid? NormalizationAggregateId { get; set; }

    internal TaskCompletionSource BeforeLocalFence { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    internal TaskCompletionSource ReleaseLocalFence { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    internal TaskCompletionSource Entered { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    internal TaskCompletionSource Release { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    internal TaskCompletionSource<ZLinkAggregateCommitResult> Settled { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    internal static SpotWideNormalizationGate Install(
        ZLinkLocationRegistration locations,
        ZLinkAuthorityKey? spotBeforeLocalFence = null
    )
    {
        var repository = Create<IZLinkLocationRepository, SpotWideNormalizationGate>();
        var gate = (SpotWideNormalizationGate)repository;
        gate._store = locations.ResolveStore()!;
        gate._spotBeforeLocalFence = spotBeforeLocalFence;
        locations._repository = repository;
        return gate;
    }

    internal static SpotWideNormalizationGate InstallSourceReadGate(
        ZLinkLocationRegistration locations,
        ZLinkAuthorityKey spot,
        ulong objectGeneration,
        RoutingId targetRid
    )
    {
        var gate = Install(locations, spot);
        gate._sourceReadTarget = (objectGeneration, targetRid);
        return gate;
    }

    protected override object? Invoke(MethodInfo? method, object?[]? args)
    {
        if (
            method!.Name == nameof(IZLinkLocationRepository.ReadAuthorityAsync)
            && _spotBeforeLocalFence is { } spot
            && (ZLinkAuthorityKey)args![0]! == spot
            && !ReleaseLocalFence.Task.IsCompleted
        )
            return ReadBeforeLocalFenceAsync(spot, (CancellationToken)args[1]!);
        if (
            _sourceReadTarget is null
            && method.Name == nameof(IZLinkLocationRepository.CommitAggregateForRecoveryAsync)
            && args![0] is ZLinkAggregateFence fence
            && (NormalizationAggregateId is null || NormalizationAggregateId == fence.AggregateId)
        )
            return CommitNormalizationAsync(fence, (CancellationToken)args[1]!);
        try
        {
            return method.Invoke(_store, args);
        }
        catch (TargetInvocationException exception) when (exception.InnerException is not null)
        {
            ExceptionDispatchInfo.Throw(exception.InnerException);
            throw;
        }
    }

    private async ValueTask<ZLinkAuthorityReadResult> ReadBeforeLocalFenceAsync(
        ZLinkAuthorityKey spot,
        CancellationToken cancellationToken
    )
    {
        var result = await _store.ReadAuthorityAsync(spot, cancellationToken);
        if (
            result is ZLinkAuthorityReadResult.Found { Snapshot: var snapshot }
            && (
                _sourceReadTarget is { } target
                    ? snapshot.ObjectGeneration == target.ObjectGeneration
                        && snapshot.Allocation.Descriptor.Rid == target.TargetRid
                        && snapshot.Relocation is { Target: { } fence } relocation
                        && ZLinkAuthorityRelocation.MatchesCommittedTarget(
                            snapshot,
                            relocation.RelocationId,
                            fence.AttemptGeneration
                        )
                        && Interlocked.Exchange(ref _sourceReadClaimed, 1) == 0
                    : snapshot.Relocation?.Phase == ServiceWireCodec.RelocationPhase.Committed
            )
        )
        {
            BeforeLocalFence.TrySetResult();
            await ReleaseLocalFence.Task.WaitAsync(cancellationToken);
        }
        return result;
    }

    private async ValueTask<ZLinkAggregateCommitResult> CommitNormalizationAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken
    )
    {
        Entered.TrySetResult();
        await Release.Task.WaitAsync(cancellationToken);
        var result = await _store.CommitAggregateForRecoveryAsync(fence, cancellationToken);
        Settled.TrySetResult(result);
        return result;
    }
}
