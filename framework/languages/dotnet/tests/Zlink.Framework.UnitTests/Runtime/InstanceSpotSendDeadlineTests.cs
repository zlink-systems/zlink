using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Instance_send_fixes_activation_deadline_before_resolve_without_ending_caller_wait(
        bool inferInstanceType
    )
    {
        const string spotId = "cold-send-deadline";
        var node = new PendingInstanceSendNode();
        HeldInstanceResolveStore? heldStore = null;
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            locationStoreWrapper: inner => heldStore = new(inner, spotId),
            relocationStore: new InMemoryRelocationStore(),
            meshResolverWrapper: inner => new RemoteInstanceResolver(inner)
        );
        var timeout = TimeSpan.FromMilliseconds(100);
        runtime.Registration.SpotNodes["entry"].DefaultRequestTimeout = timeout;
        try
        {
            await PublishCandidateAsync(
                RequireLocationStore(runtime),
                "remote-instance-owner",
                RoutingId.From("remote-instance-node"),
                TimeSpan.FromMinutes(1)
            );
            var client = new ZLinkSpotClient(runtime);
            var call = inferInstanceType
                ? client.SendToSpot(spotId, new ProbeRouteMessage("activate")).InstanceSpot()
                : client
                    .SendToSpot(spotId, new ProbeRouteMessage("activate"))
                    .InstanceSpot("Tests.InstanceSpot")
                    .InMesh("entry");
            var before = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            var pending = call.Async().AsTask();
            var after = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            await heldStore!.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await Task.Delay(TimeSpan.FromMilliseconds(350));
            Assert.False(pending.IsCompleted);
            heldStore.Release.TrySetResult();
            await node.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            try
            {
                Assert.InRange(
                    node.DeadlineUnixMs,
                    (ulong)(before + timeout.TotalMilliseconds),
                    (ulong)(after + timeout.TotalMilliseconds)
                );
                Assert.True(
                    node.DeadlineUnixMs < (ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
                );
                await Task.Delay(TimeSpan.FromMilliseconds(350));
                Assert.False(pending.IsCompleted);
                Assert.False(node.Token.IsCancellationRequested);
            }
            finally
            {
                node.Release.TrySetResult();
            }
            await pending.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(1, node.Submissions);
        }
        finally
        {
            heldStore?.Release.TrySetResult();
            node.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class PendingInstanceSendNode : CapturingSpotNode, IZLinkBackendSpotNode
    {
        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public ulong DeadlineUnixMs { get; private set; }
        public CancellationToken Token { get; private set; }
        public int Submissions { get; private set; }

        public async ValueTask<IReadOnlyList<Message>> ActivateInstanceSpotAsync(
            InstanceSpotActivationTarget target,
            string sourceSpotId,
            IReadOnlyList<Message> parts,
            bool request,
            ulong deadlineUnixMs,
            TimeSpan timeout,
            ReadOnlyMemory<byte> metadata,
            CancellationToken cancellationToken
        )
        {
            Assert.False(request);
            DeadlineUnixMs = deadlineUnixMs;
            Token = cancellationToken;
            Submissions++;
            Entered.TrySetResult();
            await Release.Task.WaitAsync(cancellationToken);
            return [];
        }
    }

    private sealed class RemoteInstanceResolver(IZLinkMeshNodeLocationResolver inner)
        : IZLinkMeshNodeLocationResolver
    {
        public async ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
            string meshName,
            CancellationToken cancellationToken = default
        ) =>
            (await inner.ListLiveMeshNodesAsync(meshName, cancellationToken))
                .Where(candidate => candidate.Rid == RoutingId.From("remote-instance-node"))
                .ToArray();
    }

    private sealed class HeldInstanceResolveStore(IZLinkLocationStore inner, string spotId)
        : IZLinkLocationStore
    {
        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            if (
                key
                == ZLinkProviderLocationRepository.AuthorityMetaKey(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId)
                )
            )
            {
                Entered.TrySetResult();
                await Release.Task.WaitAsync(cancellationToken);
            }
            return await inner.ReadAsync(key, cancellationToken);
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        ) => inner.WriteAsync(request, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);
    }
}
