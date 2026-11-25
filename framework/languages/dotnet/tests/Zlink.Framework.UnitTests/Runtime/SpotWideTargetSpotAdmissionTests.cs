using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Fact]
    public async Task SpotWide_target_queues_requests_to_the_committed_spot()
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
        var spotId = $"relocating-spot-{Guid.NewGuid():N}";
        var timeout = TimeSpan.FromSeconds(10);

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            true,
            relocateSpots: true,
            configureServices: services => services.AddTransient<SpotAdmissionWorkHandler>()
        );
        _ = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(spotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Timeout(timeout)
            .Async();

        var initializing = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        trace.InitializeTarget = async (_, cancellationToken) =>
        {
            initializing.TrySetResult();
            await release.Task.WaitAsync(cancellationToken);
        };
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            true,
            relocateSpots: true,
            configureServices: services => services.AddTransient<SpotAdmissionWorkHandler>()
        );
        var node = Assert.IsType<ZLinkBackendSpotNodeWrapper>(
            target.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName).Node
        );
        await WaitUntilAsync(() => node.Status().ActivePeerCount == 1);

        var relocation = source
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = TimeSpan.FromSeconds(30),
                }
            )
            .AsTask();
        try
        {
            await initializing.Task.WaitAsync(timeout);
            var result = await target
                .Runtime.Registration.Locations.ResolveStore()!
                .ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId),
                    CancellationToken.None
                );
            var snapshot = Assert.IsType<ZLinkAuthorityReadResult.Found>(result).Snapshot;
            Assert.Equal(node.RoutingId, snapshot.Allocation.Descriptor.Rid);
            Assert.Equal(ServiceWireCodec.RelocationPhase.Committed, snapshot.Relocation?.Phase);

            ObserveAuthority(snapshot.AuthorityOwnerGeneration);
            var first = Request("first");
            await first.Admitted.AsTask().WaitAsync(timeout);
            var second = Request("second");
            await second.Admitted.AsTask().WaitAsync(timeout);

            ObserveAuthority(checked(snapshot.AuthorityOwnerGeneration + 1));
            var stale = Request("stale");
            var failure = await Assert.ThrowsAsync<ZLinkRequestTerminalException>(() =>
                stale.Reply.AsTask()
            );
            Assert.Equal(RequestResult.NotFound, failure.Result);
            ObserveAuthority(snapshot.AuthorityOwnerGeneration);

            Assert.Empty(trace.DeliveredMarkers);

            release.TrySetResult();
            using var firstReply = await first.Reply;
            using var secondReply = await second.Reply;
            Assert.Equal(new[] { "first", "second" }, trace.DeliveredMarkers);

            void ObserveAuthority(ulong authorityOwnerGeneration) =>
                node.ObserveSpotAuthority(
                    node.RoutingId,
                    spotId,
                    snapshot.ObjectGeneration,
                    snapshot.Allocation.DescriptorLifecycleGeneration,
                    authorityOwnerGeneration,
                    checked((ulong)snapshot.OwnerLeaseGeneration)
                );

            ZLinkRequest<ZLinkBackendRouteReceived> Request(string marker) =>
                ZLinkRequestAdmission.Run(async admission =>
                {
                    var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(
                        ZLinkClientCallCodec.CreateEnvelope(
                            TimeProvider.System,
                            ZLinkMessageKind.Request,
                            RelocationBehaviorHost.MeshName,
                            nameof(SpotAdmissionWork),
                            timeout
                        ),
                        new SpotAdmissionWork(marker),
                        codecs: null
                    );
                    try
                    {
                        return await node.RequestOperations.RequestToSpotDirectAsync(
                            string.Empty,
                            node.RoutingId,
                            spotId,
                            snapshot.ObjectGeneration,
                            parts,
                            SendFlags.None,
                            default,
                            timeout,
                            CancellationToken.None,
                            requestAdmission: admission
                        );
                    }
                    finally
                    {
                        ZLinkMessageParts.DisposeAll(parts);
                    }
                });
        }
        finally
        {
            release.TrySetResult();
            await ((Task)relocation).ConfigureAwait(
                ConfigureAwaitOptions.SuppressThrowing
                    | ConfigureAwaitOptions.ContinueOnCapturedContext
            );
        }
        Assert.Equal(ZLinkFrameworkRelocationOutcome.Relocated, (await relocation).Outcome);
    }

    private sealed record SpotAdmissionWork(string Marker);

    private sealed class SpotAdmissionWorkHandler(RelocationBehaviorTrace trace)
        : IZLinkSpotRequestHandler<BehaviorTargetSpot, SpotAdmissionWork, BehaviorAck>
    {
        public ValueTask<BehaviorAck> HandleAsync(
            BehaviorTargetSpot spot,
            IZLinkMessageContext context,
            SpotAdmissionWork request,
            CancellationToken cancellationToken
        )
        {
            trace.RecordDelivery(request.Marker, "target");
            return ValueTask.FromResult(new BehaviorAck(request.Marker));
        }
    }
}
