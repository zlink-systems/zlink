using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Theory]
    [InlineData(false, 0)]
    [InlineData(true, 1)]
    public async Task Standalone_actor_preflight_reads_only_finishing_target_authority(
        bool hasFinishingTarget,
        int expectedReads
    )
    {
        var actorId = $"preflight-incoming-{Guid.NewGuid():N}";
        var steadyActorIds = Enumerable
            .Range(0, 4)
            .Select(_ => $"preflight-steady-{Guid.NewGuid():N}")
            .ToArray();
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseSourceLeave.TrySetResult();
        using var flowListener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var store = new ActorAuthorityCountingStore(
            new ZLinkInMemoryProviderLocationStore(),
            actorId,
            steadyActorIds
        );
        var relocationStore = new SynchronizedRelocationStore();
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            relocationStore,
            registerTargetSpot: false
        );
        var sourceActorIds = hasFinishingTarget ? [actorId] : steadyActorIds;
        foreach (var sourceActorId in sourceActorIds)
            await CreatePreflightActorAsync(source, sourceActorId);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            relocationStore,
            registerTargetSpot: false
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

        Task<ZLinkFrameworkRelocationResult>? relocation = null;
        try
        {
            if (hasFinishingTarget)
            {
                store.HoldSteadyWriteOnNode = target.LocalNodeRid;
                relocation = source
                    .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                    .RelocateAsync(
                        new ZLinkFrameworkRelocationOptions
                        {
                            Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                            Deadline = TimeSpan.FromSeconds(30),
                        }
                    )
                    .AsTask();
                await store.SteadyWriteStarted.Task.WaitAsync(TimeSpan.FromSeconds(10));
            }

            var preflightRuntime = (hasFinishingTarget ? target : source).Runtime;
            using var preflightDeadline = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await store.CountReadsAsync(() =>
                preflightRuntime.PreflightRetireAsync(
                    ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    preflightRuntime.Registration.ApplicationVersion,
                    preflightDeadline.Token
                )
            );

            Assert.Equal(
                hasFinishingTarget ? ZLinkFrameworkRelocationReason.OperationInProgress : null,
                result.Reason
            );
            Assert.True(
                result.Reads == expectedReads,
                $"Standalone Actor authority reads: expected {expectedReads}, actual {result.Reads}."
            );
        }
        finally
        {
            store.ReleaseSteadyWrite.TrySetResult();
            if (relocation is not null)
                await relocation;
        }
    }

    private static async Task CreatePreflightActorAsync(RelocationBehaviorHost host, string actorId)
    {
        Assert.IsType<ZLinkActorCreateResult.Created>(
            await host
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async()
        );
    }

    private sealed class ActorAuthorityCountingStore(
        IZLinkLocationStore inner,
        string incomingActorId,
        string[] steadyActorIds
    ) : IZLinkLocationStore
    {
        private readonly ZLinkStoreKey _incomingAuthorityKey =
            ZLinkProviderLocationRepository.AuthorityMetaKey(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(incomingActorId)
            );
        private readonly HashSet<ZLinkStoreKey> _actorAuthorityKeys = steadyActorIds
            .Append(incomingActorId)
            .Select(ZLinkActorAuthorityPayloadCodec.AuthorityKey)
            .Select(ZLinkProviderLocationRepository.AuthorityMetaKey)
            .ToHashSet();
        private readonly AsyncLocal<ReadCount?> _current = new();
        internal RoutingId? HoldSteadyWriteOnNode { get; set; }
        internal TaskCompletionSource SteadyWriteStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource ReleaseSteadyWrite { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal async ValueTask<(
            ZLinkFrameworkRelocationReason? Reason,
            int Reads
        )> CountReadsAsync(Func<ValueTask<ZLinkFrameworkRelocationReason?>> preflight)
        {
            var count = new ReadCount();
            _current.Value = count;
            try
            {
                return (await preflight(), count.Value);
            }
            finally
            {
                _current.Value = null;
            }
        }

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            if (_current.Value is { } count && _actorAuthorityKeys.Contains(key))
                Interlocked.Increment(ref count.Value);
            return inner.ReadAsync(key, cancellationToken);
        }

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            if (
                HoldSteadyWriteOnNode is { } targetRid
                && request
                    .Mutations.OfType<ZLinkStoreMutation.Put>()
                    .SingleOrDefault(put => put.Key == _incomingAuthorityKey)
                    is { } mutation
            )
            {
                using var document = JsonDocument.Parse(mutation.Bytes);
                var meta = document.RootElement;
                if (
                    (
                        !meta.TryGetProperty("aggregate", out var aggregate)
                        || !aggregate.TryGetProperty("relocation", out _)
                    )
                    && ZLinkActorAuthorityPayloadCodec.TryDecode(
                        meta.GetProperty("payload").GetBytesFromBase64(),
                        out var authority
                    )
                    && authority.NodeRid == targetRid
                )
                {
                    SteadyWriteStarted.TrySetResult();
                    await ReleaseSteadyWrite.Task.WaitAsync(cancellationToken);
                }
            }
            return await inner.WriteAsync(request, cancellationToken);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);

        private sealed class ReadCount
        {
            internal int Value;
        }
    }
}
