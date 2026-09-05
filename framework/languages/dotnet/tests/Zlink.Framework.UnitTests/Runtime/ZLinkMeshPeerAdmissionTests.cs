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
            identityMatch.Endpoint,
            ZLinkServiceConnectionDirection.Outbound);

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
            "tcp://unconfigured",
            ZLinkServiceConnectionDirection.Outbound);

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
            new Dictionary<RoutingId, ZLinkMeshPeer>
            {
                [sourceRid] = admitted
            },
            [admitted],
            sourceRid,
            ServiceWireConstants.Command.Hello,
            admitted.Endpoint,
            ZLinkServiceConnectionDirection.Outbound);

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
            outbound.Endpoint,
            ZLinkServiceConnectionDirection.Outbound);

        Assert.Same(outbound, selected);
    }

    [Fact]
    public void Hello_on_inbound_candidate_does_not_reuse_outbound_intent()
    {
        var sourceRid = RoutingId.From("remote-node");
        var outbound = Peer(1, "tcp://remote", sourceRid);
        var matcher = new ZLinkMeshPeerAdmission();

        var selected = matcher.FindForAdmission(
            new Dictionary<RoutingId, ZLinkMeshPeer>(),
            [outbound],
            sourceRid,
            ServiceWireConstants.Command.Hello,
            outbound.Endpoint,
            ZLinkServiceConnectionDirection.Inbound);

        Assert.Null(selected);
    }

    [Fact]
    public void Ready_candidate_uses_configured_endpoint_for_outbound_direction()
    {
        var sourceRid = RoutingId.From("remote-node");
        var outbound = Peer(1, "tcp://remote", sourceRid);

        Assert.Same(outbound, ZLinkMeshPeerAdmission.FindReadyOutboundCandidate(
            [outbound],
            outbound.Endpoint));
        Assert.Null(ZLinkMeshPeerAdmission.FindReadyOutboundCandidate(
            [outbound],
            "tcp://different"));
    }

    [Fact]
    public void Handshake_candidate_prefers_direction_and_falls_back_to_unilateral_connection()
    {
        var sourceRid = RoutingId.From("remote-node");
        var candidates = new ZLinkMeshConnectionCandidates();
        candidates.Ready(
            sourceRid,
            11,
            ZLinkServiceConnectionDirection.Outbound,
            "tcp://outbound");

        var unilateral = candidates.ForHandshake(
            sourceRid,
            ZLinkServiceConnectionDirection.Inbound);
        Assert.NotNull(unilateral);
        Assert.Equal(ZLinkServiceConnectionDirection.Outbound, unilateral.Direction);

        candidates.Ready(
            sourceRid,
            13,
            ZLinkServiceConnectionDirection.Inbound,
            "tcp://inbound");
        var bilateral = candidates.ForHandshake(
            sourceRid,
            ZLinkServiceConnectionDirection.Inbound);
        Assert.NotNull(bilateral);
        Assert.Equal(13UL, bilateral.ConnectionId);
        Assert.Equal(ZLinkServiceConnectionDirection.Inbound, bilateral.Direction);
    }

    private static ZLinkMeshPeer Peer(
        ulong intent,
        string endpoint,
        RoutingId? expectedRid) =>
        new(
            intent,
            endpoint,
            expectedRid,
            ZLinkServiceSecurityIdentity.Plaintext,
            ZLinkServiceConnectionDirection.Outbound);
}
