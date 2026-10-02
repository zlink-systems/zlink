using System.Reflection;
using Systems.Zlink;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class AutoConnectRejectionTests
{
    [Theory]
    [InlineData("provider")]
    [InlineData("cancellation")]
    [InlineData("shutdown")]
    public async Task NonConnect_Failures_Propagate_Without_Rejection_Reporting(string kind)
    {
        var fixture = await Fixture.CreateAsync();
        fixture.Resolver.Rows = fixture.Peers;
        Exception failure = kind switch
        {
            "cancellation" => new OperationCanceledException(),
            "shutdown" => new ObjectDisposedException("node"),
            _ => new InvalidOperationException("provider failure"),
        };
        fixture.Node.OverrideFailure = failure;
        await using var loop = fixture.Loop;

        var observed = await Record.ExceptionAsync(() => fixture.Reconciler.TickAsync().AsTask());

        Assert.Same(failure, observed);
        Assert.Empty(fixture.Errors.Failures);
        Assert.Single(fixture.Node.Attempts);
    }

    [Fact]
    public async Task Busy_During_Startup_Reports_Original_Error_And_Retries_Next_Tick()
    {
        var fixture = await Fixture.CreateAsync();
        fixture.Resolver.Rows = fixture.Peers;
        await using var loop = fixture.Loop;

        await loop.StartAsync();

        Assert.Same(fixture.Node.Failure, Assert.Single(fixture.Errors.Failures));
        Assert.Equal(["tcp://busy:1", "tcp://other:1"], fixture.Node.Attempts);
        Assert.Single(fixture.Reconciler.ActiveTargets);
        Assert.True(fixture.Reconciler.HasPendingTargets);

        await loop.TickAsync();

        Assert.Equal(["tcp://busy:1", "tcp://other:1", "tcp://busy:1"], fixture.Node.Attempts);
        Assert.Equal(2, fixture.Reconciler.ActiveTargets.Count);
        Assert.False(fixture.Reconciler.HasPendingTargets);
        Assert.Single(fixture.Errors.Failures);
    }

    [Fact]
    public async Task Busy_During_Worker_Tick_Keeps_Loop_And_Other_Peer_Discovery_Running()
    {
        var fixture = await Fixture.CreateAsync();
        await using var loop = fixture.Loop;
        await loop.StartAsync();

        fixture.Resolver.Rows = fixture.Peers;
        await fixture.Store.ClaimLiveOwnerAsync("busy-owner", TimeSpan.FromMinutes(1));
        await fixture.Store.UpdateMeshNodeAsync(
            fixture.Peers[0] with
            {
                LeaseGeneration = 2,
            },
            ZLinkLocationWriteIntent.NewClaim
        );
        loop.Wake();
        await fixture.Node.OtherConnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Same(fixture.Node.Failure, Assert.Single(fixture.Errors.Failures));

        loop.Wake();
        await fixture.Node.BusyConnected.Task.WaitAsync(TimeSpan.FromSeconds(5));

        await loop.StopAsync();
        Assert.Equal(["tcp://busy:1", "tcp://other:1", "tcp://busy:1"], fixture.Node.Attempts);
        Assert.Single(fixture.Errors.Failures);
    }

    private sealed record Fixture(
        ZLinkAutoConnectLoop Loop,
        ZLinkAutoConnectReconciler Reconciler,
        PeerResolver Resolver,
        IReadOnlyList<ZLinkMeshNodeDescriptor> Peers,
        ZLinkInMemoryLocationStore Store,
        BusySpotNode Node,
        AuditRuntimeFailureReporter Errors
    )
    {
        internal static async Task<Fixture> CreateAsync()
        {
            var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.FromHours(1) };
            var store = new ZLinkInMemoryLocationStore();
            var runtime = new ZLinkLocationRuntime(options, store);
            await runtime.RenewOwnerLeaseOnceAsync();
            var node = DispatchProxy.Create<IZLinkBackendSpotNode, BusySpotNode>();
            var resolver = new PeerResolver();
            var errors = new AuditRuntimeFailureReporter();
            var local = new ZLinkAutoConnectLocal(
                ZLinkLocationAutoConnectType.ClientServer,
                ZLinkMeshName.FromBoundary("play", "meshName"),
                ZLinkLocationRole.Dealer,
                null,
                string.Empty
            );
            var reconciler = new ZLinkAutoConnectReconciler(
                local,
                null,
                runtime,
                resolver,
                new ConnectorExecutor(
                    new ZLinkSpotPeerConnector(node, new ZLinkSpotPeerConnectionSet())
                ),
                options,
                errorSink: errors
            );
            return new Fixture(
                new ZLinkAutoConnectLoop(errors, reconciler, local, options, store),
                reconciler,
                resolver,
                [
                    InMemoryLocationStoreTests.MeshNode("busy-owner", "tcp://busy:1", "busy"),
                    InMemoryLocationStoreTests.MeshNode("other-owner", "tcp://other:1", "other"),
                ],
                store,
                (BusySpotNode)(object)node,
                errors
            );
        }
    }

    private sealed class PeerResolver : IZLinkMeshNodeLocationResolver
    {
        internal IReadOnlyList<ZLinkMeshNodeDescriptor> Rows { get; set; } = [];

        public ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
            string meshName,
            CancellationToken cancellationToken = default
        ) => ValueTask.FromResult(Rows);
    }

    private sealed class ConnectorExecutor(ZLinkSpotPeerConnector connector)
        : IZLinkAutoConnectExecutor
    {
        public bool Connect(ZLinkAutoConnectTarget target) =>
            connector.ConnectPeerAuto(target.NodeRid, target.Endpoint, "none");

        public bool Disconnect(ZLinkAutoConnectTarget target) => true;
    }

    private class BusySpotNode : DispatchProxy
    {
        internal ZlinkConnectException Failure { get; } = new(ZlinkConnectException.ErrorCode.Busy);
        internal Exception? OverrideFailure { get; set; }
        internal List<string> Attempts { get; } = [];
        internal TaskCompletionSource OtherConnected { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource BusyConnected { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            if (targetMethod?.Name != nameof(IZLinkBackendSpotNode.ConnectPeer))
                throw new NotSupportedException(targetMethod?.Name);
            var endpoint = (string)args![1]!;
            Attempts.Add(endpoint);
            if (endpoint == "tcp://busy:1" && Attempts.Count == 1)
                throw OverrideFailure ?? Failure;
            if (endpoint == "tcp://busy:1")
                BusyConnected.TrySetResult();
            else
                OtherConnected.TrySetResult();
            return null;
        }
    }
}
