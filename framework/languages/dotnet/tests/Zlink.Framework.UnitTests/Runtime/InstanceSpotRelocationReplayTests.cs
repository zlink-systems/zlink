using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;
using Wire = Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    private const string RelocatedRoomType = "Tests.InstanceSpot";
    private static readonly Wire.DecodeContext RelocatedRoomWire = new(
        null,
        null,
        null,
        4096,
        4096
    );

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task InstanceSpotRelocationReplaysSavedWorkAndBoundaryRelay(bool relay)
    {
        var node = new JoinedActivationNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            instanceSpotType: typeof(RelocatedRoomSpot)
        );
        var ct = CancellationToken.None;
        try
        {
            var target = runtime.GetSpotNodeRuntime("entry");
            var prepared = await target.Catalog.PrepareInstanceReservedAsync(
                RelocatedRoomType,
                "room",
                17,
                29,
                ct,
                relocationTarget: true
            );
            try
            {
                var activation = prepared.Activation;
                var seal = Assert.IsType<ZLinkSpotRelocationSeal>(
                    await activation.TrySealRelocationAsync()
                );
                var envelopeBytes = Wire.EncodeRelocationEnvelopeV1(
                    new(
                        new(new(1), new(2)),
                        new Wire.RelocationObjectIdentityCase2(
                            Wire.StatefulObjectKind.InstanceSpot,
                            new(RelocatedRoomType),
                            new("room"),
                            new(17)
                        ),
                        new(0),
                        new([
                            new(new(1), new Wire.RelocationApplicationStateCase0(Wire.Bool8.False)),
                        ]),
                        new(
                            relay
                                ? []
                                :
                                [
                                    new(new(1), new(1), RelocatedRoomRecord(false)),
                                    new(new(1), new(2), RelocatedRoomRecord(true)),
                                ]
                        ),
                        new([]),
                        new([]),
                        new([])
                    ),
                    RelocatedRoomWire
                );
                var envelope = ZLinkRelocationEnvelopeCodec.Decode(envelopeBytes);
                var stage = new TargetStage(
                    target,
                    prepared,
                    envelope,
                    [],
                    RelocatedRoomType,
                    "entry",
                    RoutingId.From("target"),
                    13,
                    new("target-owner", 23),
                    "store-version",
                    "room-relocation",
                    1,
                    TimeSpan.MaxValue,
                    [],
                    seal,
                    new Dictionary<ZLinkActorId, ulong>(),
                    19,
                    29,
                    "entry",
                    31,
                    37,
                    1
                );
                if (relay)
                {
                    var data = new ZLinkServiceWireCodec.RelocationDataRecord(
                        new(1, 2),
                        1,
                        new("target-owner", 23, RoutingId.From("target"), 13, "store-version"),
                        1,
                        new(
                            (byte)ZLinkPlacementObjectKind.InstanceSpot,
                            RelocatedRoomType,
                            "room",
                            17,
                            0
                        ),
                        new(
                            Wire.EncodeFrozenRecord(
                                RelocatedRoomRecord(false, multipart: true),
                                RelocatedRoomWire
                            )
                        )
                    );
                    var request = data with
                    {
                        FrozenRecord = new(
                            Wire.EncodeFrozenRecord(
                                RelocatedRoomRecord(true, multipart: true),
                                RelocatedRoomWire
                            )
                        ),
                    };
                    foreach (var record in new[] { data, request })
                    {
                        Assert.True(
                            ZLinkServiceWireCodec.TryDecodeRelocationData(
                                ZLinkServiceWireCodec.EncodeRelocationData(record),
                                out var decoded,
                                out _
                            )
                        );
                        await stage.AppendCanonicalInboundDataAsync(decoded);
                    }
                    stage.ValidateBoundary(
                        new(
                            data.RelocationId,
                            1,
                            data.Coordinator,
                            1,
                            data.Object,
                            2,
                            ZLinkRelocationBoundaryBatch.ComputeChecksum([
                                data.FrozenRecord.Encoded,
                                request.FrozenRecord.Encoded,
                            ])
                        )
                    );
                }
                var spot = Assert.IsType<RelocatedRoomSpot>(activation.Spot);
                Assert.Empty(spot.Deliveries);
                var replies = new List<ulong>();
                var deliveries = new List<Task>();
                foreach (
                    var job in stage
                        .SpotParticipant.AcceptedJobs.Concat(stage.SnapshotHeldRecords())
                        .OrderBy(static job => job.AcceptedSequence)
                )
                {
                    var journal = ZLinkSpotActivation.DecodeRelocationReplayRecord(job, "entry");
                    Assert.Equal("v", journal.Metadata.Values["k"]);
                    Assert.Equal("source-room", journal.SpotId);
                    var delivery = activation.AdmitAcceptedReplayJob(
                        job,
                        "entry",
                        (reply, _) =>
                        {
                            Assert.NotNull(reply);
                            Assert.Equal(RoutingId.From("source"), journal.SourceNodeRid);
                            Assert.Equal(new MeshOperationId(1UL << 63, 37), journal.OperationId);
                            Assert.Equal(41UL, journal.RequestSequence);
                            var replyParts = reply.Select(Message.From).ToArray();
                            Assert.Equal(
                                "request",
                                ZLinkClientCallCodec
                                    .DecodeEnvelopeReplyAndDispose<ProbeReply>(
                                        replyParts,
                                        "empty relocation reply",
                                        "failed relocation reply",
                                        runtime.Registration.Codecs
                                    )
                                    .Value
                            );
                            replies.Add(journal.ReplyRouteId);
                            return ValueTask.CompletedTask;
                        }
                    );
                    if (delivery is not null)
                        deliveries.Add(delivery);
                }
                Assert.Empty(spot.Deliveries);
                Assert.True(
                    await activation.OpenRelocationTargetAdmissionAsync(
                        seal,
                        static () => { },
                        static () => ValueTask.CompletedTask
                    )
                );
                await Task.WhenAll(deliveries).WaitAsync(TimeSpan.FromSeconds(30), ct);
                Assert.Equal(new[] { "send", "request" }, spot.Deliveries);
                Assert.Equal(new[] { 41UL }, replies);
            }
            finally
            {
                await target.Catalog.DiscardReservedAsync(prepared);
            }
        }
        finally
        {
            await runtime.StopAsync(ct);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void InstanceSpotRelocationSourceWritesOriginalReadyOperation(bool request)
    {
        var route = RelocatedRoomRoute();
        using var header = ZLinkEnvelopeCodec.EncodeHeader(
            new(
                request ? ZLinkMessageKind.Request : ZLinkMessageKind.Command,
                "entry",
                "room-packet",
                "application/octet-stream",
                request ? "41" : null,
                null,
                null,
                null,
                null,
                "source-room"
            )
        );
        using var body = Message.From(new byte[] { 3, 5, 8 });
        var source = RoutingId.From("source");
        var operation = request ? new MeshOperationId(1UL << 63, 37) : default;
        using var received = new ZLinkBackendRouteReceived(
            [header, body],
            source,
            "source-room",
            request ? 41UL : null,
            request ? _ => SubmitResult.Ok : null,
            operationId: operation,
            targetNodeGeneration: 13,
            authorityOwnerGeneration: 19,
            ownerLeaseGeneration: 23,
            sourceNodeGeneration: 7,
            requestSource: new("source-owner", 11, source, 7),
            readyInstanceRoute: route
        );
        var jobs = ZLinkSpotRetireScheduler.ResolveFrozenAcceptedJobs([
            new(1, ZLinkSpotAcceptedJournal.Encode(received, request ? 41UL : 0)),
        ]);
        var inventory = new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            new byte[32],
            [
                new(
                    new("spot:room"),
                    ZLinkPlacementObjectKind.InstanceSpot,
                    17,
                    19,
                    default,
                    jobs,
                    []
                ),
            ]
        );

        var saved = ZLinkCanonicalSpotRelocationWriter.CreateInitial(
            inventory,
            "room",
            RelocatedRoomType,
            route.TargetNodeRid.RoutingId,
            0
        );
        var wire = Wire.DecodeFrozenRecord(
            Assert.Single(Assert.Single(saved.Participants).AcceptedJobs).Payload.ToArray(),
            RelocatedRoomWire
        );
        Assert.Equal(Wire.MeshRecordKind.InstanceSpotActivation, wire.RecordKind);
        Assert.Equal(
            request ? Wire.MeshOperationKind.InstanceSpotRequest : Wire.MeshOperationKind.None,
            wire.OperationKind
        );
        Assert.Equal(operation.High, wire.OperationId.High.Value);
        Assert.Equal(operation.Low, wire.OperationId.Low.Value);
        var instance = Assert.IsType<Wire.FrozenRecordBodyCase13>(wire.Body);
        Assert.Equal(
            route.Authority,
            Assert.IsType<Wire.InstanceRouteV1Case0>(instance.Route).Authority
        );
        Assert.Equal(
            request ? Wire.InstanceOperationKind.Request : Wire.InstanceOperationKind.Send,
            instance.OperationKind
        );
    }

    private static Wire.InstanceRouteV1Case0 RelocatedRoomRoute() =>
        new(
            Wire.InstanceRouteKind.Ready,
            new(RoutingId.From("target")),
            new(13),
            new("room"),
            new(new(17), new("target-owner"), new(19), new(23), new("store-version")),
            Wire.Bool8.True
        );

    private static Wire.FrozenRecord RelocatedRoomRecord(
        bool request,
        bool javaInput = false,
        bool multipart = false
    )
    {
        // The source fence, Ready route, metadata and operation values are
        // shared with Java ZLinkInstanceSpotFrozenRecordTest.
        var payload = request
            ? """{"value":"request"}"""u8.ToArray()
            : """{"value":"send"}"""u8.ToArray();
        var packetName = request ? "room-query" : "room-packet";
        Wire.ApplicationPayloadEnvelopeV1 application;
        if (javaInput)
            application = new(new("room-packet"), new("application/octet-stream"), new([3, 5, 8]));
        else if (multipart)
        {
            using var header = ZLinkEnvelopeCodec.EncodeHeader(
                new(
                    request ? ZLinkMessageKind.Request : ZLinkMessageKind.Command,
                    "entry",
                    packetName,
                    "application/json",
                    request ? "41" : null,
                    null,
                    null,
                    null,
                    null,
                    "source-room"
                )
            );
            using var body = Message.From(payload);
            application = Wire.DecodeApplicationPayloadEnvelopeV1(
                ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                    new[] { header, body }
                ),
                RelocatedRoomWire
            );
        }
        else
            application = new(new(packetName), new("application/json"), new(payload));
        return new(
            Wire.MeshRecordKind.InstanceSpotActivation,
            new Wire.FrozenSourceIdentityCase1(
                Wire.FrozenSourceKind.Spot,
                new(RoutingId.From("source")),
                new(7),
                new("source-owner"),
                new(11),
                new("source-room")
            ),
            Wire.Bool8.True,
            new([new(new("k"), new("v"))]),
            new(new(request ? 1UL << 63 : 0), new(request ? 37UL : 0)),
            request ? Wire.MeshOperationKind.InstanceSpotRequest : Wire.MeshOperationKind.None,
            request
                ? new Wire.FrozenReplyRouteCase4(new(41))
                : new Wire.FrozenReplyRouteOtherwise(),
            new Wire.FrozenRecordBodyCase13(
                RelocatedRoomRoute(),
                new(7),
                request ? Wire.InstanceOperationKind.Request : Wire.InstanceOperationKind.Send,
                application
            )
        );
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void InstanceSpotRelocationReadsJavaReferenceFrozenRecord(bool request)
    {
        var encoded = Wire.EncodeFrozenRecord(
            RelocatedRoomRecord(request, javaInput: true),
            RelocatedRoomWire
        );
        var job = ZLinkRelocationEnvelopeCodec.DecodeCanonicalSavedWork(
            1,
            encoded,
            new((byte)Wire.StatefulObjectKind.InstanceSpot, RelocatedRoomType, "room", 17, 0)
        );
        var projection = Assert.IsType<ZLinkCanonicalAcceptedRequest>(job.CanonicalRequest);
        Assert.Equal("room", projection.TargetSpotId);
        Assert.Equal(17UL, projection.TargetSpotGeneration);
        Assert.Equal(RoutingId.From("source").ToHex(), projection.Source.NodeRid);
        Assert.Equal(7UL, projection.Source.NodeGeneration);
        Assert.Equal("source-room", projection.SourceSpotId);
        Assert.Equal(request ? 41UL : 0, projection.ReplyRouteId);
        Assert.Equal("room-packet", projection.ApplicationPayload.PacketName);
        Assert.Equal("application/octet-stream", projection.ApplicationPayload.ContentType);
        Assert.Equal(new byte[] { 3, 5, 8 }, projection.ApplicationPayload.Payload.ToArray());
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void InstanceSpotRelocationRejectsAnOperationIdThatDisagreesWithTheInnerKind(
        bool request
    )
    {
        var wire = RelocatedRoomRecord(request, javaInput: true) with
        {
            OperationId = new(new(0), new(request ? 0UL : 1UL)),
        };
        var encoded = Wire.EncodeFrozenRecord(wire, RelocatedRoomWire);

        Assert.False(ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(encoded));
        Assert.Throws<InvalidDataException>(() =>
            ZLinkRelocationEnvelopeCodec.DecodeCanonicalSavedWork(
                1,
                encoded,
                new((byte)Wire.StatefulObjectKind.InstanceSpot, RelocatedRoomType, "room", 17, 0)
            )
        );
    }

    private sealed class RelocatedRoomSpot(IZLinkInstanceSpotContext context) : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context { get; } = context;
        internal List<string> Deliveries { get; } = [];

        public void Configure()
        {
            Context.Handlers.AddPacket<RelocatedRoomSend>();
            Context.Handlers.AddPacket<RelocatedRoomRequest>();
        }
    }

    [ZLinkPacket("room-packet")]
    private sealed record RelocatedRoomMessage(string Value);

    [ZLinkPacket("room-query")]
    private sealed record RelocatedRoomQuery(string Value);

    private sealed class RelocatedRoomSend
        : IZLinkSpotPacketHandler<RelocatedRoomSpot, RelocatedRoomMessage>
    {
        public ValueTask HandleAsync(
            RelocatedRoomSpot spot,
            IZLinkMessageContext context,
            RelocatedRoomMessage message,
            CancellationToken cancellationToken
        )
        {
            spot.Deliveries.Add(message.Value);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class RelocatedRoomRequest
        : IZLinkSpotRequestHandler<RelocatedRoomSpot, RelocatedRoomQuery, ProbeReply>
    {
        public ValueTask<ProbeReply> HandleAsync(
            RelocatedRoomSpot spot,
            IZLinkMessageContext context,
            RelocatedRoomQuery message,
            CancellationToken cancellationToken
        )
        {
            spot.Deliveries.Add(message.Value);
            return ValueTask.FromResult(new ProbeReply(message.Value));
        }
    }
}
