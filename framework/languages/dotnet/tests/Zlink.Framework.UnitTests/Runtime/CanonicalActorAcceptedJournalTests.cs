using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class CanonicalActorAcceptedJournalTests
{
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void Java_multipart_record_round_trips_the_wire_identity(bool request)
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var path = Path.GetFullPath(
            "../../runtime/protocol/golden/actor-accepted-record-v1.json",
            frameworkRoot
        );
        using var fixture = JsonDocument.Parse(File.ReadAllText(path));
        var bytes = Convert.FromHexString(
            fixture.RootElement.GetProperty(request ? "requestHex" : "sendHex").GetString()!
        );
        var transfer = ServiceWirePilotCodec.EncodeRelocationEnvelopeV1(
            new ServiceWirePilotCodec.RelocationEnvelopeV1(
                1,
                2,
                new ServiceWirePilotCodec.RelocationActorIdentity("accepted-actor", 3, 4),
                0,
                [new ServiceWirePilotCodec.RelocationApplicationState(1, false, [])],
                [new ServiceWirePilotCodec.RelocationSavedWork(1, 9, bytes)],
                [],
                [],
                []
            )
        );
        var records = Assert
            .Single(ZLinkRelocationEnvelopeCodec.Decode(transfer).Participants)
            .AcceptedJobs;
        var record = Assert.Single(records);
        var accepted = Assert.Single(
            ZLinkStandaloneActorRelocationRuntime.DecodeAcceptedRecords(records)
        );
        var frame = accepted.Frame;
        var target = Assert.IsType<ZLinkBackendActorRef>(accepted.FrozenTargetActor);

        Assert.Equal(9, frame.ArrivalIndex);
        Assert.Equal(9UL, (frame with { ArrivalIndex = 0 }).CanonicalAcceptedSequence);
        Assert.Equal("accepted-actor", target.ActorId);
        Assert.Equal(RoutingId.From(new byte[] { 2, 0, 128 }), target.NodeRid);
        Assert.Equal(target.NodeRid.ToBytes().ToArray(), frame.ReplyActorNodeRid);
        Assert.Equal(target.Generation, frame.ReplyActorGeneration);
        Assert.Equal(RoutingId.From(new byte[] { 0, 255, 1 }), accepted.RequestSource.NodeRid);
        Assert.Equal(accepted.RequestSource.NodeRid.ToHex(), record.RequestSource?.NodeRid);
        Assert.Equal(target.NodeRid.ToHex(), record.CanonicalRequest?.TargetNodeRid);
        Assert.Equal((ulong)(request ? 37 : 0), frame.RelocationReplyRouteId);
        Assert.Equal(
            new MeshOperationId(31, request ? 41UL : 42UL),
            frame.RouteContext.OperationId
        );
        Assert.Equal(
            "accepted-packet",
            ZLinkStreamProtocolDefaults.DecodeHeader(frame.Header).Name
        );
        Assert.Equal(new byte[] { 0, 255, 3 }, frame.Body);
        if (request)
            accepted = accepted with
            {
                Frame = frame with
                {
                    RouteContext = frame.RouteContext with { ReplyCapability = "source-callback" },
                },
            };
        Assert.Equal(bytes, ZLinkCanonicalActorAcceptedJournal.Encode(accepted, target));
    }

    [Fact]
    public void Request_round_trip_preserves_operation_reply_and_authority_fences()
    {
        var source = SourceFence("source-owner", 41);
        var target = new ZLinkBackendActorRef(RoutingId.From("target-node"), "actor-1", 17);
        var header = ZLinkStreamProtocolDefaults.EncodeHeader(
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.None,
                new ZlinkStreamRequestSeq(71),
                "game.lookup",
                ZlinkStreamMetadata.Empty,
                CorrelationId: "correlation-1"
            )
        );
        var frame = new ZLinkActorHandoffFrame(
            Zlink.Framework.Runtime.Dispatch.ZLinkApplicationJobOrigin.Remote,
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
                ReplyCapability: "actor-reply"
            ),
            source.NodeGeneration,
            RelocationReplyRouteId: 73
        );

        var encoded = ZLinkCanonicalActorAcceptedJournal.Encode(
            new ZLinkActorAcceptedRecord(frame, source),
            target
        );
        var decoded = ZLinkCanonicalActorAcceptedJournal.Decode(encoded, frame.ArrivalIndex);

        Assert.True(ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(encoded));
        Assert.Equal(source, decoded.Source);
        Assert.Equal(source, decoded.Frame.RequestSource);
        Assert.Equal(target, decoded.TargetActor);
        Assert.Equal(frame.RouteContext.OperationId, decoded.Frame.RouteContext.OperationId);
        Assert.Equal<ulong>(73, decoded.Frame.RouteContext.ReplyRequestId);
        Assert.Equal<ulong>(73, decoded.Frame.RelocationReplyRouteId);
        Assert.Equal<ulong>(23, decoded.Frame.RouteContext.TargetNodeGeneration);
        Assert.Equal<ulong>(29, decoded.Frame.RouteContext.AuthorityOwnerGeneration);
        Assert.Equal<ulong>(31, decoded.Frame.RouteContext.OwnerLeaseGeneration);
        Assert.Equal(frame.Header, decoded.Frame.Header);
        Assert.Equal(frame.Body, decoded.Frame.Body);
        Assert.Equal(frame.SourceSessionRid, decoded.Frame.SourceSessionRid);
        Assert.Equal(frame.ReplyActorNodeRid, decoded.Frame.ReplyActorNodeRid);
    }

    [Fact]
    public void Aggregate_preserves_distinct_request_source_fences()
    {
        var first = SourceFence("source-owner-a", 41, "source-node-a", 37);
        var second = SourceFence("source-owner-b", 42, "source-node-b", 38);
        var target = new ZLinkBackendActorRef(RoutingId.From("target-node"), "actor-1", 17);

        var decoded = new[] { first, second }
            .Select(
                (source, index) =>
                    ZLinkCanonicalActorAcceptedJournal.Decode(
                        ZLinkCanonicalActorAcceptedJournal.Encode(
                            new ZLinkActorAcceptedRecord(Frame(source), source),
                            target
                        ),
                        index + 1
                    )
            )
            .ToArray();

        Assert.Equal(first, decoded[0].Accepted.RequestSource);
        Assert.Equal(second, decoded[1].Accepted.RequestSource);
        Assert.NotEqual(decoded[0].Accepted.RequestSource, decoded[1].Accepted.RequestSource);
    }

    [Fact]
    public void Bound_session_one_way_round_trip_allows_a_lagging_actor_generation_copy()
    {
        var source = SourceFence("session-owner", 41);
        var target = new ZLinkBackendActorRef(RoutingId.From("target-node"), "actor-1", 17);
        var bound = new ZLinkActorBoundSessionHandoffFence(
            target.ActorId,
            target.Generation - 1,
            RoutingId.From("session-1"),
            "binding-token",
            43,
            47
        );
        var header = ZLinkStreamProtocolDefaults.EncodeHeader(
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                "move",
                ZlinkStreamMetadata.Empty
            )
        );
        var frame = new ZLinkActorHandoffFrame(
            Zlink.Framework.Runtime.Dispatch.ZLinkApplicationJobOrigin.Remote,
            [],
            0,
            source.NodeRid.ToBytes().ToArray(),
            bound.SessionRid.ToBytes().ToArray(),
            0,
            0,
            header.ToArray(),
            [1, 2, 3],
            1,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(53, 59),
                0,
                61,
                67,
                71,
                IsBoundSessionRoute: true
            ),
            source.NodeGeneration,
            source,
            BoundSessionSource: bound
        );

        var encoded = ZLinkCanonicalActorAcceptedJournal.Encode(
            new ZLinkActorAcceptedRecord(frame, source, target),
            target
        );
        var decoded = ZLinkCanonicalActorAcceptedJournal.Decode(encoded, 1);

        Assert.True(ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(encoded));
        Assert.Equal(bound, decoded.Frame.BoundSessionSource);
        Assert.NotEqual(bound.ActorGeneration, decoded.TargetActor.Generation);
        Assert.Equal(target.Generation, decoded.TargetActor.Generation);
        Assert.Equal(new MeshOperationId(53, 59), decoded.Frame.RouteContext.OperationId);
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
                new ZLinkBackendActorRef(RoutingId.From("target-node"), "actor-1", 17)
            )
        );
    }

    [Fact]
    public void Encode_rejects_frame_and_request_source_mismatch()
    {
        var source = SourceFence("source-owner", 41);
        var replacement = SourceFence("replacement-owner", 42, "replacement-node", 43);

        Assert.Throws<InvalidOperationException>(() =>
            ZLinkCanonicalActorAcceptedJournal.Encode(
                new ZLinkActorAcceptedRecord(Frame(source), replacement),
                new ZLinkBackendActorRef(RoutingId.From("target-node"), "actor-1", 17)
            )
        );
    }

    [Fact]
    public void Late_reply_ack_requires_the_decoded_request_source_fence()
    {
        var source = SourceFence("source-owner", 41);
        var target = new ZLinkBackendActorRef(RoutingId.From("target-node"), "actor-1", 17);
        var decoded = ZLinkCanonicalActorAcceptedJournal.Decode(
            ZLinkCanonicalActorAcceptedJournal.Encode(
                new ZLinkActorAcceptedRecord(Frame(source), source),
                target
            ),
            1
        );
        var exact = decoded.Accepted.RequestSource;

        Assert.True(
            ZLinkManagedMeshNode.IsExactReplyRelayAckSource(
                exact.NodeRid,
                exact.NodeGeneration,
                exact,
                exact
            )
        );
        Assert.False(
            ZLinkManagedMeshNode.IsExactReplyRelayAckSource(
                exact.NodeRid,
                exact.NodeGeneration,
                exact,
                exact with
                {
                    LeaseGeneration = exact.LeaseGeneration + 1,
                }
            )
        );
        Assert.False(
            ZLinkManagedMeshNode.IsExactReplyRelayAckSource(
                exact.NodeRid,
                exact.NodeGeneration,
                exact,
                exact with
                {
                    NodeGeneration = exact.NodeGeneration + 1,
                }
            )
        );
    }

    [Fact]
    public void Decode_rejects_malformed_record()
    {
        Assert.Throws<InvalidDataException>(() =>
            ZLinkCanonicalActorAcceptedJournal.Decode([9, 1, 0], 1)
        );
    }

    private static ZLinkActorHandoffFrame Frame(ZLinkServiceWireCodec.RequestSourceFence source)
    {
        var header = ZLinkStreamProtocolDefaults.EncodeHeader(
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                new ZlinkStreamRequestSeq(1),
                "packet",
                ZlinkStreamMetadata.Empty
            )
        );
        return new ZLinkActorHandoffFrame(
            Zlink.Framework.Runtime.Dispatch.ZLinkApplicationJobOrigin.Remote,
            [],
            0,
            source.NodeRid.ToBytes().ToArray(),
            [],
            7,
            1,
            header.ToArray(),
            [1],
            1,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(1, 2),
                0,
                3,
                4,
                5,
                ReplyRequestId: 7
            ),
            source.NodeGeneration,
            RelocationReplyRouteId: 7
        );
    }

    private static ZLinkServiceWireCodec.RequestSourceFence SourceFence(
        string ownerId,
        ulong lease,
        string nodeRid = "source-node",
        ulong nodeGeneration = 37
    ) => new(ownerId, lease, RoutingId.From(nodeRid), nodeGeneration);
}
