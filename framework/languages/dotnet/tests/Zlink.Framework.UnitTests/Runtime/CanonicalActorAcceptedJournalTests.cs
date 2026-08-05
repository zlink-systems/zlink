using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class CanonicalActorAcceptedJournalTests
{
    [Fact]
    public void Request_round_trip_preserves_operation_reply_and_authority_fences()
    {
        var source = SourceFence("source-owner", 41);
        var target = new ZLinkBackendActorRef(
            RoutingId.From("target-node"), "actor-1", 17);
        var header = ZLinkStreamProtocolDefaults.EncodeHeader(
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.None,
                new ZlinkStreamRequestSeq(71),
                "game.lookup",
                ZlinkStreamMetadata.Empty,
                CorrelationId: "correlation-1"));
        var frame = new ZLinkActorHandoffFrame(
            RoutingId.From("reply-node").ToBytes().ToArray(),
            19,
            source.NodeRid.ToBytes().ToArray(),
            RoutingId.From("session-1").ToBytes().ToArray(),
            73,
            1,
            header.ToArray(),
            [1, 2, 3, 4],
            9,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(0x1122334455667788, 0x99aabbccddeeff00),
                2,
                23,
                29,
                31,
                ReplyRequestId: 73,
                ReplyFlags: 5,
                ReplyCapability: "actor-reply"),
            source.NodeGeneration,
            RelocationReplyRouteId: 73);

        var encoded = ZLinkCanonicalActorAcceptedJournal.Encode(
            new ZLinkActorAcceptedRecord(frame, source), target);
        var decoded = ZLinkCanonicalActorAcceptedJournal.Decode(
            encoded, frame.ArrivalIndex);

        Assert.True(ZLinkRelocationEnvelopeCodec
            .TryValidateCanonicalFrozenRecord(encoded));
        Assert.Equal(source, decoded.Source);
        Assert.Equal(source, decoded.Frame.RequestSource);
        Assert.Equal(target, decoded.TargetActor);
        Assert.Equal(frame.RouteContext.OperationId,
            decoded.Frame.RouteContext.OperationId);
        Assert.Equal<ulong>(73, decoded.Frame.RouteContext.ReplyRequestId);
        Assert.Equal<ulong>(73, decoded.Frame.RelocationReplyRouteId);
        Assert.Equal<ulong>(23,
            decoded.Frame.RouteContext.TargetNodeGeneration);
        Assert.Equal<ulong>(29,
            decoded.Frame.RouteContext.AuthorityOwnerGeneration);
        Assert.Equal<ulong>(31,
            decoded.Frame.RouteContext.OwnerLeaseGeneration);
        Assert.Equal(frame.Header, decoded.Frame.Header);
        Assert.Equal(frame.Body, decoded.Frame.Body);
        Assert.Equal(frame.SourceSessionRid, decoded.Frame.SourceSessionRid);
        Assert.Equal(frame.ReplyActorNodeRid,
            decoded.Frame.ReplyActorNodeRid);
    }

    [Fact]
    public void Aggregate_preserves_distinct_request_source_fences()
    {
        var first = SourceFence("source-owner-a", 41, "source-node-a", 37);
        var second = SourceFence("source-owner-b", 42, "source-node-b", 38);
        var target = new ZLinkBackendActorRef(
            RoutingId.From("target-node"), "actor-1", 17);

        var decoded = new[] { first, second }
            .Select((source, index) =>
                ZLinkCanonicalActorAcceptedJournal.Decode(
                    ZLinkCanonicalActorAcceptedJournal.Encode(
                        new ZLinkActorAcceptedRecord(Frame(source), source),
                        target),
                    index + 1))
            .ToArray();

        Assert.Equal(first, decoded[0].Accepted.RequestSource);
        Assert.Equal(second, decoded[1].Accepted.RequestSource);
        Assert.NotEqual(decoded[0].Accepted.RequestSource,
            decoded[1].Accepted.RequestSource);
    }

    [Fact]
    public void Bound_session_one_way_round_trip_preserves_same_generation_and_sequence()
    {
        var source = SourceFence("session-owner", 41);
        var target = new ZLinkBackendActorRef(
            RoutingId.From("target-node"), "actor-1", 17);
        var bound = new ZLinkActorBoundSessionHandoffFence(
            target.ActorId,
            target.Generation,
            RoutingId.From("session-1"),
            "binding-token",
            43,
            47);
        var header = ZLinkStreamProtocolDefaults.EncodeHeader(
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                "move",
                ZlinkStreamMetadata.Empty));
        var frame = new ZLinkActorHandoffFrame(
            [], 0,
            source.NodeRid.ToBytes().ToArray(),
            bound.SessionRid.ToBytes().ToArray(),
            0, 0, header.ToArray(), [1, 2, 3], 1,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(53, 59), 0, 61, 67, 71,
                IsBoundSessionRoute: true),
            source.NodeGeneration,
            source,
            BoundSessionSource: bound);

        var encoded = ZLinkCanonicalActorAcceptedJournal.Encode(
            new ZLinkActorAcceptedRecord(frame, source, target), target);
        var decoded = ZLinkCanonicalActorAcceptedJournal.Decode(encoded, 1);

        Assert.True(ZLinkRelocationEnvelopeCodec
            .TryValidateCanonicalFrozenRecord(encoded));
        Assert.Equal(bound, decoded.Frame.BoundSessionSource);
        Assert.Equal(target.Generation, decoded.TargetActor.Generation);
        Assert.Equal(new MeshOperationId(53, 59),
            decoded.Frame.RouteContext.OperationId);
        Assert.True(decoded.Frame.RouteContext.IsBoundSessionRoute);
        Assert.False(decoded.Frame.RouteContext.IsDirectRoute);
        Assert.Equal(frame.Body, decoded.Frame.Body);
    }

    [Fact]
    public void Encode_rejects_missing_source_fence()
    {
        var source = SourceFence("source-owner", 41);
        var frame = Frame(source);

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkCanonicalActorAcceptedJournal.Encode(
                new ZLinkActorAcceptedRecord(frame, default),
                new ZLinkBackendActorRef(
                    RoutingId.From("target-node"), "actor-1", 17)));
    }

    [Fact]
    public void Encode_rejects_frame_and_request_source_mismatch()
    {
        var source = SourceFence("source-owner", 41);
        var replacement = SourceFence(
            "replacement-owner", 42, "replacement-node", 43);

        Assert.Throws<InvalidOperationException>(() =>
            ZLinkCanonicalActorAcceptedJournal.Encode(
                new ZLinkActorAcceptedRecord(Frame(source), replacement),
                new ZLinkBackendActorRef(
                    RoutingId.From("target-node"), "actor-1", 17)));
    }

    [Fact]
    public void Late_reply_ack_requires_the_decoded_request_source_fence()
    {
        var source = SourceFence("source-owner", 41);
        var target = new ZLinkBackendActorRef(
            RoutingId.From("target-node"), "actor-1", 17);
        var decoded = ZLinkCanonicalActorAcceptedJournal.Decode(
            ZLinkCanonicalActorAcceptedJournal.Encode(
                new ZLinkActorAcceptedRecord(Frame(source), source), target),
            1);
        var exact = decoded.Accepted.RequestSource;

        Assert.True(ZLinkManagedMeshNode.IsExactReplyRelayAckSource(
            exact.NodeRid, exact.NodeGeneration, exact, exact));
        Assert.False(ZLinkManagedMeshNode.IsExactReplyRelayAckSource(
            exact.NodeRid, exact.NodeGeneration, exact,
            exact with { LeaseGeneration = exact.LeaseGeneration + 1 }));
        Assert.False(ZLinkManagedMeshNode.IsExactReplyRelayAckSource(
            exact.NodeRid, exact.NodeGeneration, exact,
            exact with { NodeGeneration = exact.NodeGeneration + 1 }));
    }

    [Fact]
    public void Decode_rejects_malformed_record()
    {
        Assert.Throws<InvalidDataException>(() =>
            ZLinkCanonicalActorAcceptedJournal.Decode([9, 1, 0], 1));
    }

    private static ZLinkActorHandoffFrame Frame(
        ZLinkServiceWireCodec.RequestSourceFence source)
    {
        var header = ZLinkStreamProtocolDefaults.EncodeHeader(
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                new ZlinkStreamRequestSeq(1),
                "packet",
                ZlinkStreamMetadata.Empty));
        return new ZLinkActorHandoffFrame(
            [], 0, source.NodeRid.ToBytes().ToArray(), [],
            7, 1, header.ToArray(), [1], 1,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(1, 2), 0, 3, 4, 5,
                ReplyRequestId: 7),
            source.NodeGeneration,
            RelocationReplyRouteId: 7);
    }

    private static ZLinkServiceWireCodec.RequestSourceFence SourceFence(
        string ownerId,
        ulong lease,
        string nodeRid = "source-node",
        ulong nodeGeneration = 37) => new(
        ownerId,
        lease,
        RoutingId.From(nodeRid),
        nodeGeneration);
}
