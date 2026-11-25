using System.Reflection;
using System.Runtime.ExceptionServices;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Fact]
    public Task SpotWide_find_follows_target_dispatch_before_steady_normalization() =>
        RunSpotWideFindReadinessAsync(verifyClose: false);

    [Fact]
    public Task SpotWide_close_reports_retryable_moving_before_steady_normalization() =>
        RunSpotWideFindReadinessAsync(verifyClose: true);

    private static async Task RunSpotWideFindReadinessAsync(bool verifyClose)
    {
        using var listener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseTargetLifecycle.TrySetResult();
        var store = new ZLinkInMemoryProviderLocationStore();
        var roots = new SynchronizedRelocationStore();
        var spotId = $"find-ready-{Guid.NewGuid():N}";
        var timeout = TimeSpan.FromSeconds(10);
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
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
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            true,
            relocateSpots: true
        );
        var locations = target.Runtime.Registration.Locations;
        var normalization = SpotWideNormalizationGate.Install(locations);
        var authorityStore = new ZLinkProviderLocationRepository(store);
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
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
            var progress = ZLinkAuthorityAggregateCodec
                .Replace(null, committed.Relocation)!
                .Value.GetProperty("relocation")
                .GetProperty("completion");
            Assert.Equal("0", progress.GetProperty("acceptedRequests").GetString());
            Assert.Equal("0", progress.GetProperty("completedResults").GetString());
            Assert.Equal("0", progress.GetProperty("pendingDeliveries").GetString());
            var spots = target.Services.GetRequiredService<IZLinkSpotManager>();
            var beforeDispatch = await spots.FindAsync(spotId);

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

            if (verifyClose)
            {
                var moving = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                    spots.CloseAsync(opened).AsTask()
                );
                Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, moving.Kind);
                Assert.Equal(ZLinkRetryAdvice.RetryAfterBackoff, moving.RetryAdvice);
                Assert.Equal(
                    (int)ServiceWireConstants.FrameworkErrorCode.SpotMoving,
                    moving.FrameworkFailureCode
                );
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
                Assert.True(await spots.CloseAsync(opened));
        }
        finally
        {
            releaseInitialization.TrySetResult();
            normalization.Release.TrySetResult();
            cancellation.Cancel();
            await ((Task)relocation).ConfigureAwait(
                ConfigureAwaitOptions.SuppressThrowing
                    | ConfigureAwaitOptions.ContinueOnCapturedContext
            );
        }
    }
}

public class SpotWideNormalizationGate : DispatchProxy
{
    private IZLinkLocationRepository _store = null!;
    private ZLinkAuthorityKey? _spotBeforeLocalFence;

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

    protected override object? Invoke(MethodInfo? method, object?[]? args)
    {
        if (
            method!.Name == nameof(IZLinkLocationRepository.ReadAuthorityAsync)
            && _spotBeforeLocalFence is { } spot
            && (ZLinkAuthorityKey)args![0]! == spot
            && !ReleaseLocalFence.Task.IsCompleted
        )
            return ReadBeforeLocalFenceAsync(spot, (CancellationToken)args[1]!);
        if (method.Name == nameof(IZLinkLocationRepository.CommitAggregateForRecoveryAsync))
            return CommitNormalizationAsync(
                (ZLinkAggregateFence)args![0]!,
                (CancellationToken)args[1]!
            );
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
            result is ZLinkAuthorityReadResult.Found
            {
                Snapshot.Relocation.Phase: ServiceWireCodec.RelocationPhase.Committed,
            }
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
