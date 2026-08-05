using System.Reflection;
using Systems.Zlink;
using Zlink.Framework.Runtime.Backend.Contracts;
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

        Assert.False(connector.ConnectPeerAuto(
            RoutingId.From("peer"),
            "tcp://peer:1",
            "none"));
        Assert.True(connector.ConnectPeerAuto(
            RoutingId.From("peer"),
            "tcp://peer:1",
            "none"));
        Assert.Equal(2, proxy.ConnectAttempts);
    }

    [Fact]
    public void Auto_Router_Replaces_A_Different_Rid_At_The_Same_Endpoint()
    {
        var node = DispatchProxy.Create<IZLinkBackendSpotNode, ReplacementSpotNode>();
        var proxy = (ReplacementSpotNode)(object)node;
        var connector = new ZLinkSpotPeerConnector(
            node,
            new ZLinkSpotPeerConnectionSet());
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

        Assert.True(connector.DisconnectPeerAuto(newRid, "tcp://peer:1"));
        Assert.Equal(2, proxy.DisconnectedEndpoints.Count);
    }

    [Fact]
    public void Auto_NonInitiator_Delegates_Admission_Pending_Cleanup()
    {
        var node = DispatchProxy.Create<IZLinkBackendSpotNode, CleanupSpotNode>();
        var proxy = (CleanupSpotNode)(object)node;
        var connector = new ZLinkSpotPeerConnector(
            node,
            new ZLinkSpotPeerConnectionSet());
        var peerRid = RoutingId.From("peer");

        Assert.True(connector.DisconnectPeerBeforeAdmission(
            peerRid,
            "tcp://peer:1",
            lifecycleGeneration: 7));
        Assert.Equal(
            (peerRid, "tcp://peer:1", 7UL),
            proxy.Cleanup);
    }

    private class BusyOnceSpotNode : DispatchProxy
    {
        internal int ConnectAttempts { get; private set; }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            if (targetMethod.Name != nameof(IZLinkBackendSpotNode.ConnectPeer))
                throw new NotSupportedException(targetMethod.Name);

            ConnectAttempts++;
            if (ConnectAttempts == 1)
                throw new ZlinkConnectException(ZlinkConnectException.ErrorCode.Busy);

            return null;
        }
    }

    private class CleanupSpotNode : DispatchProxy
    {
        internal (RoutingId Rid, string Endpoint, ulong Generation)? Cleanup {
            get;
            private set;
        }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            if (targetMethod.Name
                != nameof(IZLinkBackendSpotNode.DisconnectPeerBeforeAdmission))
                throw new NotSupportedException(targetMethod.Name);

            Cleanup = (
                (RoutingId)args![0]!,
                (string)args[1]!,
                (ulong)args[2]!);
            return true;
        }
    }

    private class ReplacementSpotNode : DispatchProxy
    {
        internal List<RoutingId> ConnectedRids { get; } = [];

        internal List<string> DisconnectedEndpoints { get; } = [];

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            switch (targetMethod.Name)
            {
                case nameof(IZLinkBackendSpotNode.ConnectPeer)
                    when args is { Length: 3 }:
                    ConnectedRids.Add((RoutingId)args[0]!);
                    return null;
                case nameof(IZLinkBackendSpotNode.DisconnectPeer):
                    DisconnectedEndpoints.Add((string)args![0]!);
                    return null;
                default:
                    throw new NotSupportedException(targetMethod.Name);
            }
        }
    }
}
