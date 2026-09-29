using System.Reflection;
using Systems.Zlink;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SpotPeerConnectorTests
{
    [Fact]
    public void Auto_Router_Connect_Retries_After_Busy()
    {
        var node = DispatchProxy.Create<IZLinkBackendSpotNode, BusyOnceSpotNode>();
        var proxy = (BusyOnceSpotNode)(object)node;
        var connector = new ZLinkSpotPeerConnector(node, new ZLinkSpotPeerConnectionSet());

        Assert.False(connector.ConnectPeerAuto(RoutingId.From("peer"), "tcp://peer:1", "none"));
        Assert.True(connector.ConnectPeerAuto(RoutingId.From("peer"), "tcp://peer:1", "none"));
        Assert.Equal(2, proxy.ConnectAttempts);
    }

    [Fact]
    public void Auto_Router_Replaces_A_Different_Rid_At_The_Same_Endpoint()
    {
        var node = DispatchProxy.Create<IZLinkBackendSpotNode, ReplacementSpotNode>();
        var proxy = (ReplacementSpotNode)(object)node;
        var connector = new ZLinkSpotPeerConnector(node, new ZLinkSpotPeerConnectionSet());
        var oldRid = RoutingId.From("old-peer");
        var newRid = RoutingId.From("new-peer");

        Assert.True(connector.ConnectPeerAuto(oldRid, "tcp://peer:1", "none"));
        Assert.True(connector.ConnectPeerAuto(newRid, "tcp://peer:1", "none"));

        Assert.Equal([oldRid, newRid], proxy.ConnectedRids);
        Assert.Equal(["tcp://peer:1"], proxy.DisconnectedEndpoints);

        // Removing the stale target must not tear down the replacement
        // connection that now owns this endpoint.
        Assert.True(connector.DisconnectPeerAuto(oldRid, "tcp://peer:1"));
        Assert.Single(proxy.DisconnectedEndpoints);
        Assert.Equal((oldRid, "tcp://peer:1", 1UL), proxy.AdmissionCleanup);

        Assert.True(connector.DisconnectPeerAuto(newRid, "tcp://peer:1"));
        Assert.Single(proxy.DisconnectedEndpoints);
        Assert.Equal((newRid, "tcp://peer:1", 1UL), proxy.AdmissionCleanup);
    }

    [Fact]
    public void Auto_NonInitiator_Delegates_Admission_Pending_Cleanup()
    {
        var node = DispatchProxy.Create<IZLinkBackendSpotNode, CleanupSpotNode>();
        var proxy = (CleanupSpotNode)(object)node;
        var connector = new ZLinkSpotPeerConnector(node, new ZLinkSpotPeerConnectionSet());
        var peerRid = RoutingId.From("peer");

        Assert.True(
            connector.DisconnectPeerBeforeAdmission(peerRid, "tcp://peer:1", lifecycleGeneration: 7)
        );
        Assert.Equal((peerRid, "tcp://peer:1", 7UL), proxy.Cleanup);
    }

    private class BusyOnceSpotNode : DispatchProxy
    {
        internal int ConnectAttempts { get; private set; }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            if (
                targetMethod.Name
                is not nameof(IZLinkBackendSpotNode.ConnectPeer)
                    and not nameof(IZLinkBackendSpotNode.ConnectPeerAsync)
            )
                throw new NotSupportedException(targetMethod.Name);

            ConnectAttempts++;
            if (ConnectAttempts == 1)
                throw new ZlinkConnectException(ZlinkConnectException.ErrorCode.Busy);

            return targetMethod.Name == nameof(IZLinkBackendSpotNode.ConnectPeerAsync)
                ? ValueTask.CompletedTask
                : null;
        }
    }

    private class CleanupSpotNode : DispatchProxy
    {
        internal (RoutingId Rid, string Endpoint, ulong Generation)? Cleanup { get; private set; }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            if (
                targetMethod.Name
                is not nameof(IZLinkBackendSpotNode.DisconnectPeerBeforeAdmission)
                    and not nameof(IZLinkBackendSpotNode.DisconnectPeerBeforeAdmissionAsync)
            )
                throw new NotSupportedException(targetMethod.Name);

            Cleanup = ((RoutingId)args![0]!, (string)args[1]!, (ulong)args[2]!);
            return
                targetMethod.Name
                == nameof(IZLinkBackendSpotNode.DisconnectPeerBeforeAdmissionAsync)
                ? ValueTask.FromResult(true)
                : true;
        }
    }

    private class ReplacementSpotNode : DispatchProxy
    {
        internal List<RoutingId> ConnectedRids { get; } = [];

        internal List<string> DisconnectedEndpoints { get; } = [];

        internal (RoutingId Rid, string Endpoint, ulong Generation)? AdmissionCleanup
        {
            get;
            private set;
        }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            switch (targetMethod.Name)
            {
                case nameof(IZLinkBackendSpotNode.ConnectPeer) when args is { Length: 3 }:
                case nameof(IZLinkBackendSpotNode.ConnectPeerAsync) when args is { Length: 3 }:
                    ConnectedRids.Add((RoutingId)args[0]!);
                    return targetMethod.Name == nameof(IZLinkBackendSpotNode.ConnectPeerAsync)
                        ? ValueTask.CompletedTask
                        : null;
                case nameof(IZLinkBackendSpotNode.DisconnectPeer):
                case nameof(IZLinkBackendSpotNode.DisconnectPeerAsync):
                    DisconnectedEndpoints.Add((string)args![0]!);
                    return targetMethod.Name == nameof(IZLinkBackendSpotNode.DisconnectPeerAsync)
                        ? ValueTask.CompletedTask
                        : null;
                case nameof(IZLinkBackendSpotNode.MeshPeers):
                case nameof(IZLinkBackendSpotNode.MeshPeersAsync):
                    var peers = ConnectedRids
                        .Select(
                            (rid, index) =>
                                new MeshNodePeer(
                                    ConnectionIntentId: (ulong)index + 1,
                                    Source: MeshPeerSource.Discovery,
                                    State: MeshPeerState.Connecting,
                                    RoutingId: rid,
                                    LifecycleGeneration: 1,
                                    DescriptorRevision: 1,
                                    Endpoint: "tcp://peer:1",
                                    ChannelCount: 0,
                                    LastError: 0,
                                    LastChangedMs: 0
                                )
                        )
                        .ToArray();
                    if (targetMethod.Name == nameof(IZLinkBackendSpotNode.MeshPeersAsync))
                        return ValueTask.FromResult<IReadOnlyList<MeshNodePeer>>(peers);
                    return peers;
                case nameof(IZLinkBackendSpotNode.DisconnectPeerBeforeAdmission):
                case nameof(IZLinkBackendSpotNode.DisconnectPeerBeforeAdmissionAsync):
                    AdmissionCleanup = ((RoutingId)args![0]!, (string)args[1]!, (ulong)args[2]!);
                    return
                        targetMethod.Name
                        == nameof(IZLinkBackendSpotNode.DisconnectPeerBeforeAdmissionAsync)
                        ? ValueTask.FromResult(true)
                        : true;
                default:
                    throw new NotSupportedException(targetMethod.Name);
            }
        }
    }
}
