using Systems.Zlink;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ZLinkMeshPeerAdmissionTests
{
    [Fact]
    public void Admission_prefers_configured_identity_over_endpoint_order()
    {
        var sourceRid = RoutingId.From("remote-node");
        var endpointMatch = Peer(
            1,
            "tcp://endpoint-match",
            expectedRid: null);
        var identityMatch = Peer(
            2,
            "tcp://identity-match",
            expectedRid: sourceRid);
        var matcher = new ZLinkMeshPeerAdmission();

        var selected = matcher.FindForAdmission(
            new Dictionary<RoutingId, ZLinkMeshPeer>(),
            [endpointMatch, identityMatch],
            sourceRid,
            ServiceWireConstants.Command.Admit,
            identityMatch.Endpoint);

        Assert.Same(identityMatch, selected);
    }

    [Fact]
    public void Admission_does_not_guess_when_unknown_intents_are_ambiguous()
    {
        var matcher = new ZLinkMeshPeerAdmission();
        var selected = matcher.FindForAdmission(
            new Dictionary<RoutingId, ZLinkMeshPeer>(),
            [
                Peer(1, "tcp://first", expectedRid: null),
                Peer(2, "tcp://second", expectedRid: null)
            ],
            RoutingId.From("remote-node"),
            ServiceWireConstants.Command.Admit,
            "tcp://unconfigured");

        Assert.Null(selected);
    }

    [Fact]
    public void Duplicate_selection_uses_admitted_rid_before_endpoint_fallback()
    {
        var sourceRid = RoutingId.From("remote-node");
        var admitted = Peer(1, "tcp://old", expectedRid: sourceRid);
        admitted.RoutingId = sourceRid;
        admitted.Admitted = true;
        var incoming = Peer(2, admitted.Endpoint, expectedRid: null);
        var matcher = new ZLinkMeshPeerAdmission();

        var duplicate = matcher.FindDuplicate(
            new Dictionary<RoutingId, ZLinkMeshPeer>
            {
                [sourceRid] = admitted
            },
            [admitted, incoming],
            sourceRid,
            incoming);

        Assert.Same(admitted, duplicate);
    }

    private static ZLinkMeshPeer Peer(
        ulong intent,
        string endpoint,
        RoutingId? expectedRid) =>
        new(
            intent,
            endpoint,
            expectedRid,
            "none",
            ZLinkServiceConnectionDirection.Outbound);
}
