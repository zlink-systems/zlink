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
                Assert.True(target.Runtime.SnapshotOperationAdmissions().ActiveCount > 0);
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
                settlement.Release.TrySetResult();
                await cutover!.WaitAsync(TimeSpan.FromSeconds(5));
                await settlement.Settled.Task.WaitAsync(TimeSpan.FromSeconds(5));
                // Location runtime §10: a definitive conflict is also settlement.
                // Sealing publishes Draining before this delayed Spot CAS, so
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
        int order
    )
    {
        var repository = Create<IZLinkLocationRepository, ShutdownSettlementRepository>();
        var proxy = (ShutdownSettlementRepository)repository;
        proxy._inner = registration.ResolveStore()!;
        proxy._order = order;
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
