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
        var endpointMatch = Peer(1, "tcp://endpoint-match", expectedRid: null);
        var identityMatch = Peer(2, "tcp://identity-match", expectedRid: sourceRid);
        var matcher = new ZLinkMeshPeerAdmission();

        var selected = matcher.FindForAdmission(
            new Dictionary<RoutingId, ZLinkMeshPeer>(),
            [endpointMatch, identityMatch],
            sourceRid,
            ServiceWireConstants.Command.Admit,
            identityMatch.Endpoint
        );

        Assert.Same(identityMatch, selected);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Admit_on_selected_inbound_pair_matches_current_logical_outbound_intent(
        bool alreadyAdmitted
    )
    {
        var sourceRid = RoutingId.From("remote-node");
        var outbound = Peer(1, "tcp://remote", sourceRid);
        outbound.RoutingId = sourceRid;
        outbound.Admitted = alreadyAdmitted;
        var peersByRid = new Dictionary<RoutingId, ZLinkMeshPeer>();
        if (alreadyAdmitted)
            peersByRid.Add(sourceRid, outbound);
        var matcher = new ZLinkMeshPeerAdmission();

        Assert.Same(
            outbound,
            matcher.FindForAdmission(
                peersByRid,
                [outbound],
                sourceRid,
                ServiceWireConstants.Command.Admit,
                outbound.Endpoint
            )
        );
        Assert.Null(
            matcher.FindForAdmission(
                new Dictionary<RoutingId, ZLinkMeshPeer>(),
                [],
                sourceRid,
                ServiceWireConstants.Command.Admit,
                outbound.Endpoint
            )
        );
    }

    [Fact]
    public void Admission_does_not_guess_when_unknown_intents_are_ambiguous()
    {
        var matcher = new ZLinkMeshPeerAdmission();
        var selected = matcher.FindForAdmission(
            new Dictionary<RoutingId, ZLinkMeshPeer>(),
            [Peer(1, "tcp://first", expectedRid: null), Peer(2, "tcp://second", expectedRid: null)],
            RoutingId.From("remote-node"),
            ServiceWireConstants.Command.Admit,
            "tcp://unconfigured"
        );

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
            new Dictionary<RoutingId, ZLinkMeshPeer> { [sourceRid] = admitted },
            [admitted, incoming],
            sourceRid,
            incoming
        );

        Assert.Same(admitted, duplicate);
    }

    [Fact]
    public void Repeat_admission_of_live_peer_reuses_its_connection_generation()
    {
        var sourceRid = RoutingId.From("remote-node");
        var admitted = Peer(1, "tcp://remote", sourceRid);
        admitted.RoutingId = sourceRid;
        admitted.Admitted = true;
        admitted.ConnectionGeneration = 42;
        var matcher = new ZLinkMeshPeerAdmission();

        var selected = matcher.FindForAdmission(
            new Dictionary<RoutingId, ZLinkMeshPeer> { [sourceRid] = admitted },
            [admitted],
            sourceRid,
            ServiceWireConstants.Command.Hello,
            admitted.Endpoint
        );

        Assert.Same(admitted, selected);
        Assert.Equal(42UL, selected!.ConnectionGeneration);
    }

    [Fact]
    public void Hello_on_unilateral_connection_reuses_configured_outbound_intent()
    {
        var sourceRid = RoutingId.From("remote-node");
        var outbound = Peer(1, "tcp://remote", sourceRid);
        var matcher = new ZLinkMeshPeerAdmission();

        var selected = matcher.FindForAdmission(
            new Dictionary<RoutingId, ZLinkMeshPeer>(),
            [outbound],
            sourceRid,
            ServiceWireConstants.Command.Hello,
            outbound.Endpoint
        );

        Assert.Same(outbound, selected);
    }

    [Fact]
    public void Admission_does_not_bind_an_unconfigured_endpoint_to_the_sole_unknown_intent()
    {
        // An intent without an expected RID is identified only by the endpoint
        // echoed in the handshake. A handshake from another node on its own
        // selected route must not claim that intent.
        var matcher = new ZLinkMeshPeerAdmission();
        var unknown = Peer(1, "tcp://configured", expectedRid: null);

        Assert.Null(
            matcher.FindForAdmission(
                new Dictionary<RoutingId, ZLinkMeshPeer>(),
                [unknown],
                RoutingId.From("other-node"),
                ServiceWireConstants.Command.Admit,
                "tcp://other-node"
            )
        );
        Assert.Same(
            unknown,
            matcher.FindForAdmission(
                new Dictionary<RoutingId, ZLinkMeshPeer>(),
                [unknown],
                RoutingId.From("configured-node"),
                ServiceWireConstants.Command.Admit,
                unknown.Endpoint
            )
        );
    }

    [Fact]
    public void Selected_routes_report_missing_and_replaced_routes_as_ended()
    {
        var kept = RoutingId.From("kept");
        var replaced = RoutingId.From("replaced");
        var removed = RoutingId.From("removed");
        var added = RoutingId.From("added");
        var routes = new ZLinkMeshSelectedRoutes();

        Assert.Empty(routes.Apply([new(kept, 1), new(replaced, 2), new(removed, 3)]));

        var ended = routes.Apply([new(kept, 1), new(replaced, 9), new(added, 4)]);

        Assert.Equal(
            new[] { replaced, removed }.OrderBy(static rid => rid.ToHex()),
            ended.OrderBy(static rid => rid.ToHex())
        );
    }

    private static ZLinkMeshPeer Peer(ulong intent, string endpoint, RoutingId? expectedRid) =>
        new(
            intent,
            endpoint,
            expectedRid,
            ZLinkServiceSecurityIdentity.Plaintext,
            ZLinkServiceConnectionDirection.Outbound
        );
}
