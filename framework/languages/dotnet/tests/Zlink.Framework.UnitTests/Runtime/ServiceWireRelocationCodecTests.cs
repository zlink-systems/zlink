using Systems.Zlink.Framework.Runtime.Protocol;
using System.Text.Json;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class ServiceWireRelocationCodecTests
{
    [Fact]
    public void Relocation_control_commands_match_the_shared_golden_vectors()
    {
        var coordinator = Coordinator();
        var candidate = Candidate();
        var participants = Participants();
        var relocation = new ZLinkServiceWireCodec.RelocationWireId(4, 5);
        var objectRecord = Object();
        var root = new ZLinkServiceWireCodec.RelocationRootRecord(
            "relocation-root", 0x12345678);

        var ready = new ZLinkServiceWireCodec.RelocationReadyRecord(
            relocation, 6, 1, coordinator, candidate, objectRecord, 2, 2, 128,
            [], 11, 12, 13, root, 1,
            [
                new ZLinkServiceWireCodec.RelocationParticipantProgressRecord(1, 2, 0),
                new ZLinkServiceWireCodec.RelocationParticipantProgressRecord(2, 0, 0)
            ]);
        AssertGoldenRoundTrip<ZLinkServiceWireCodec.RelocationReadyRecord>("relocationReadyCapacityOffer",
            ZLinkServiceWireCodec.EncodeRelocationReady(ready),
            ZLinkServiceWireCodec.TryDecodeRelocationReady,
            ZLinkServiceWireCodec.EncodeRelocationReady);

        var data = Data(phase: 4);
        AssertGoldenRoundTrip<ZLinkServiceWireCodec.RelocationDataRecord>("relocationDataCommittedControlRecord",
            ZLinkServiceWireCodec.EncodeRelocationData(data),
            ZLinkServiceWireCodec.TryDecodeRelocationData,
            ZLinkServiceWireCodec.EncodeRelocationData,
            ZLinkServiceWireCodec.DecodeError.InvalidField);

        var ack = new ZLinkServiceWireCodec.RelocationAckRecord(
            relocation, 6, coordinator, 2, 1, 2);
        AssertGoldenRoundTrip<ZLinkServiceWireCodec.RelocationAckRecord>("relocationAckHighWater",
            ZLinkServiceWireCodec.EncodeRelocationAck(ack),
            ZLinkServiceWireCodec.TryDecodeRelocationAck,
            ZLinkServiceWireCodec.EncodeRelocationAck);

        var seal = new ZLinkServiceWireCodec.RelocationSealRecord(
            relocation, 6, coordinator, 1, true,
            [
                new ZLinkServiceWireCodec.RelocationParticipantTerminalRecord(1, 2),
                new ZLinkServiceWireCodec.RelocationParticipantTerminalRecord(2, 0)
            ]);
        AssertGoldenRoundTrip<ZLinkServiceWireCodec.RelocationSealRecord>("relocationSealResponse",
            ZLinkServiceWireCodec.EncodeRelocationSeal(seal),
            ZLinkServiceWireCodec.TryDecodeRelocationSeal,
            ZLinkServiceWireCodec.EncodeRelocationSeal);

        var complete = new ZLinkServiceWireCodec.RelocationCompleteRecord(
            relocation, 6, coordinator, 1,
            new ZLinkServiceWireCodec.RequestSourceFence(
                "source-owner", 9, RoutingId.From("node-a"), 11),
            1);
        AssertGoldenRoundTrip<ZLinkServiceWireCodec.RelocationCompleteRecord>("relocationCompleteSourceCleanup",
            ZLinkServiceWireCodec.EncodeRelocationComplete(complete),
            ZLinkServiceWireCodec.TryDecodeRelocationComplete,
            ZLinkServiceWireCodec.EncodeRelocationComplete);

        var prepare = new ZLinkServiceWireCodec.RelocationPrepareRecord(
            relocation, 6, 1, coordinator, candidate, 1, objectRecord,
            RoutingId.From("node-a"), 11, 2, 128, participants, root, 1);
        AssertGoldenRoundTrip<ZLinkServiceWireCodec.RelocationPrepareRecord>("relocationPrepareSharedRoot",
            ZLinkServiceWireCodec.EncodeRelocationPrepare(prepare),
            ZLinkServiceWireCodec.TryDecodeRelocationPrepare,
            ZLinkServiceWireCodec.EncodeRelocationPrepare);

        var reserved = new ZLinkServiceWireCodec.RelocationReservedRecord(
            relocation, 6, 1, coordinator, candidate, 13, participants);
        AssertGoldenRoundTrip<ZLinkServiceWireCodec.RelocationReservedRecord>("relocationReservedAck",
            ZLinkServiceWireCodec.EncodeRelocationReserved(reserved),
            ZLinkServiceWireCodec.TryDecodeRelocationReserved,
            ZLinkServiceWireCodec.EncodeRelocationReserved);
    }

    [Fact]
    public void Relocation_ready_roles_keep_offer_and_accept_participants_separate()
    {
        var participants = Participants();
        var common = new ZLinkServiceWireCodec.RelocationReadyRecord(
            new ZLinkServiceWireCodec.RelocationWireId(4, 5),
            6,
            1,
            Coordinator(),
            Candidate(),
            Object(),
            2,
            2,
            128,
            [],
            11,
            12,
            13,
            new ZLinkServiceWireCodec.RelocationRootRecord(
                "relocation-root", 0x12345678),
            1,
            [
                new ZLinkServiceWireCodec.RelocationParticipantProgressRecord(
                    1, 2, 0),
                new ZLinkServiceWireCodec.RelocationParticipantProgressRecord(
                    2, 0, 0)
            ]);

        _ = ZLinkServiceWireCodec.EncodeRelocationReady(common);
        _ = ZLinkServiceWireCodec.EncodeRelocationReady(common with
        {
            Role = 1,
            OfferedMessages = 0,
            OfferedBytes = 0,
            Participants = participants
        });

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeRelocationReady(common with
            {
                Participants = participants
            }));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeRelocationReady(common with
            {
                Role = 1,
                OfferedMessages = 0,
                OfferedBytes = 0
            }));
    }

    [Fact]
    public void Instance_spot_command40_uses_the_schema_normalized_object_shape()
    {
        var instance =
            ZLinkSpotRetireTargetRuntime.CreateCanonicalRelocationObject(
                ZLinkPlacementObjectKind.InstanceSpot,
                "Game.Instance",
                "instance-1",
                9,
                10);
        Assert.Equal(0UL, instance.ExpectedAuthorityOwnerGeneration);
        var prepare = new ZLinkServiceWireCodec.RelocationPrepareRecord(
            new ZLinkServiceWireCodec.RelocationWireId(4, 5),
            6,
            1,
            Coordinator(),
            Candidate(),
            1,
            instance,
            RoutingId.From("node-a"),
            11,
            0,
            64,
            Participants(),
            new ZLinkServiceWireCodec.RelocationRootRecord(
                "relocation-root", 0x12345678),
            1);

        var encoded = ZLinkServiceWireCodec.EncodeRelocationPrepare(prepare);
        Assert.True(ZLinkServiceWireCodec.TryDecodeRelocationPrepare(
            encoded, out var decoded, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(instance, decoded.Object);
        var offer = new ZLinkServiceWireCodec.RelocationReadyRecord(
            prepare.RelocationId,
            prepare.TargetAttemptGeneration,
            prepare.RoundKind,
            prepare.Coordinator,
            prepare.Candidate,
            instance,
            2,
            64,
            1024,
            [],
            prepare.SourceNodeGeneration,
            prepare.Candidate.NodeGeneration,
            1,
            prepare.Root,
            prepare.ApplicationVersion,
            prepare.Participants.Select(static participant =>
                    new ZLinkServiceWireCodec.RelocationParticipantProgressRecord(
                        participant.ParticipantId,
                        0,
                        0))
                .ToArray());
        var encodedOffer = ZLinkServiceWireCodec.EncodeRelocationReady(offer);
        Assert.True(ZLinkServiceWireCodec.TryDecodeRelocationReady(
            encodedOffer, out var decodedOffer, out error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(offer.Object, decodedOffer.Object);
        Assert.Equal(offer.RelocationId, decodedOffer.RelocationId);
        Assert.Equal(
            offer.ParticipantProgress,
            decodedOffer.ParticipantProgress);
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeRelocationPrepare(
                prepare with
                {
                    Object = instance with
                    {
                        ExpectedAuthorityOwnerGeneration = 10
                    }
                }));
    }

    [Theory]
    [InlineData(0UL)]
    [InlineData(742UL)]
    public void Bound_session_participants_round_trip_the_last_accepted_session_sequence(
        ulong lastAcceptedSessionSequence)
    {
        var participants = new[]
        {
            new ZLinkServiceWireCodec.RelocationParticipantRecord(
                1, 1, default, 0, null, 0, default, 0, 0, 2, 128),
            new ZLinkServiceWireCodec.RelocationParticipantRecord(
                2, 2, RoutingId.From("session-owner"), 5, "owner-1", 6,
                RoutingId.From("session-rid"), 7, lastAcceptedSessionSequence,
                3, 256)
        };
        var prepare = new ZLinkServiceWireCodec.RelocationPrepareRecord(
            new ZLinkServiceWireCodec.RelocationWireId(4, 5), 6, 1,
            Coordinator(), Candidate(), 1, Object(),
            RoutingId.From("node-a"), 11, 5, 384, participants,
            new ZLinkServiceWireCodec.RelocationRootRecord(
                "relocation-root", 0x12345678),
            1);

        var encoded = ZLinkServiceWireCodec.EncodeRelocationPrepare(prepare);
        Assert.True(ZLinkServiceWireCodec.TryDecodeRelocationPrepare(
            encoded, out var decoded, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(participants, decoded.Participants);
        Assert.Equal(
            lastAcceptedSessionSequence,
            decoded.Participants[1].LastAcceptedSessionSequence);
        Assert.Equal(encoded, ZLinkServiceWireCodec.EncodeRelocationPrepare(decoded));

        var reserved = new ZLinkServiceWireCodec.RelocationReservedRecord(
            prepare.RelocationId, 6, 1, prepare.Coordinator,
            prepare.Candidate, 13, participants);
        var encodedReserved =
            ZLinkServiceWireCodec.EncodeRelocationReserved(reserved);
        Assert.True(ZLinkServiceWireCodec.TryDecodeRelocationReserved(
            encodedReserved, out var decodedReserved, out error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(participants, decodedReserved.Participants);
    }

    [Fact]
    public void Bound_session_journal_commands_match_the_shared_golden_vectors()
    {
        var relocation = new ZLinkServiceWireCodec.RelocationWireId(4, 5);
        var coordinator = Coordinator();
        var candidate = Candidate();
        var participants = BoundSessionJournalParticipants();

        var prepare = new ZLinkServiceWireCodec.RelocationPrepareRecord(
            relocation, 6, 1, coordinator, candidate, 1, Object(),
            RoutingId.From("node-a"), 11, 3, 192, participants,
            new ZLinkServiceWireCodec.RelocationRootRecord(
                "relocation-root", 0x12345678),
            1);
        AssertGoldenRoundTrip<ZLinkServiceWireCodec.RelocationPrepareRecord>(
            "relocationPrepareBoundSessionJournal",
            ZLinkServiceWireCodec.EncodeRelocationPrepare(prepare),
            ZLinkServiceWireCodec.TryDecodeRelocationPrepare,
            ZLinkServiceWireCodec.EncodeRelocationPrepare);

        var reserved = new ZLinkServiceWireCodec.RelocationReservedRecord(
            relocation, 6, 1, coordinator, candidate, 13, participants);
        AssertGoldenRoundTrip<ZLinkServiceWireCodec.RelocationReservedRecord>(
            "relocationReservedBoundSessionAck",
            ZLinkServiceWireCodec.EncodeRelocationReserved(reserved),
            ZLinkServiceWireCodec.TryDecodeRelocationReserved,
            ZLinkServiceWireCodec.EncodeRelocationReserved);

        Assert.True(ZLinkServiceWireCodec.TryDecodeRelocationPrepare(
            ReadRelocationControlGolden("relocationPrepareBoundSessionJournal"),
            out var decodedPrepare, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(1, decodedPrepare.InitiatorRole);
        Assert.Equal(3UL, decodedPrepare.RequiredMessages);
        Assert.Equal(192UL, decodedPrepare.RequiredBytes);
        Assert.Equal(participants, decodedPrepare.Participants);
        AssertBoundSessionJournalParticipants(decodedPrepare.Participants);

        Assert.True(ZLinkServiceWireCodec.TryDecodeRelocationReserved(
            ReadRelocationControlGolden("relocationReservedBoundSessionAck"),
            out var decodedReserved, out error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(13UL, decodedReserved.ReservationGeneration);
        Assert.Equal(participants, decodedReserved.Participants);
        AssertBoundSessionJournalParticipants(decodedReserved.Participants);
    }

    [Fact]
    public void Relocation_data_accepts_every_closed_phase_and_rejects_unknown_phase()
    {
        for (byte phase = 0; phase <= 9; phase++)
        {
            var encoded = ZLinkServiceWireCodec.EncodeRelocationData(Data(phase));
            Assert.True(ZLinkServiceWireCodec.TryDecodeRelocationData(
                encoded, out var decoded, out var error));
            Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
            Assert.True(ZLinkServiceWireCodec.TryDecodeFrozenRelocationControl(
                decoded.FrozenRecord, out var control));
            Assert.Equal(phase, control.Phase);
        }

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeRelocationData(Data(10)));

        var phaseEight = ZLinkServiceWireCodec.EncodeRelocationData(Data(8));
        var invalid = ZLinkServiceWireCodec.EncodeRelocationData(Data(9));
        var phaseOffset = Enumerable.Range(0, invalid.Length)
            .Single(index => phaseEight[index] != invalid[index]);
        invalid[phaseOffset] = 10;
        Assert.False(ZLinkServiceWireCodec.TryDecodeRelocationData(
            invalid, out _, out var invalidError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.InvalidField, invalidError);
    }

    [Fact]
    public void Relocation_data_preserves_application_frozen_records_and_fails_closed()
    {
        var envelope = ZLinkRelocationEnvelopeCodec.Decode(
            ReadCanonicalRelocationGolden());
        var raw = Assert.Single(envelope.Participants[0].AcceptedJobs).Payload;
        var record = Data(4) with
        {
            FrozenRecord = new ZLinkServiceWireCodec.FrozenRecord(raw)
        };

        var encoded = ZLinkServiceWireCodec.EncodeRelocationData(record);
        Assert.True(ZLinkServiceWireCodec.TryDecodeRelocationData(
            encoded, out var decoded, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(raw.ToArray(), decoded.FrozenRecord.Encoded.ToArray());
        Assert.Equal(encoded, ZLinkServiceWireCodec.EncodeRelocationData(decoded));

        var unknownKind = raw.ToArray();
        unknownKind[0] = 15;
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeRelocationData(record with
            {
                FrozenRecord = new ZLinkServiceWireCodec.FrozenRecord(unknownKind)
            }));
    }

    [Fact]
    public void Reply_relay_command_33_preserves_the_exact_maintenance_fence()
    {
        var coordinator = new ZLinkServiceWireCodec.RelocationCoordinatorFence(
            "coordinator", 7, RoutingId.From("node-a"), 11, "store-3");
        var expected = new ZLinkServiceWireCodec.ReplyRelayRecord(
            new MeshOperationId(1, 2),
            3,
            new ZLinkServiceWireCodec.RelocationWireId(4, 5),
            6,
            coordinator,
            8,
            9,
            101,
            ServiceWireConstants.FrameworkErrorCode.None);

        var encoded = ZLinkServiceWireCodec.EncodeReplyRelay(expected);
        var golden = ReadGolden("maintenanceReplyRelay");

        Assert.Equal(golden, encoded);
        Assert.Equal(ServiceWireConstants.Magic0, encoded[0]);
        Assert.Equal(ServiceWireConstants.Magic1, encoded[1]);
        Assert.Equal(ServiceWireConstants.WireMajor, encoded[2]);
        Assert.Equal((byte)ServiceWireConstants.Command.ReplyRelay, encoded[3]);
        Assert.Equal((byte)ServiceWireConstants.Flag.None, encoded[4]);
        Assert.True(ZLinkServiceWireCodec.TryDecodeReplyRelay(
            encoded, out var actual, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(expected, actual);

        Assert.False(ZLinkServiceWireCodec.TryDecodeReplyRelay(
            encoded[..^1], out _, out var truncated));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TruncatedField, truncated);
        Assert.False(ZLinkServiceWireCodec.TryDecodeReplyRelay(
            [.. encoded, 0], out _, out var trailing));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TrailingByte, trailing);
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeReplyRelay(expected with
            {
                TerminalResult = 102,
                FailureCode = ServiceWireConstants.FrameworkErrorCode.None
            }));
    }

    [Fact]
    public void Reply_relay_ack_command_46_requires_a_closed_terminal_status()
    {
        var coordinator = new ZLinkServiceWireCodec.RelocationCoordinatorFence(
            "coordinator", 7, RoutingId.From("node-a"), 11, "store-3");
        var expected = new ZLinkServiceWireCodec.ReplyRelayAckRecord(
            new ZLinkServiceWireCodec.RelocationWireId(4, 5),
            coordinator,
            new MeshOperationId(1, 2),
            3,
            new ZLinkServiceWireCodec.RequestSourceFence(
                "source", 13, RoutingId.From("node-s"), 17),
            2);

        var encoded = ZLinkServiceWireCodec.EncodeReplyRelayAck(expected);
        var golden = ReadGolden("replyRelayAlreadyTerminalAck");

        Assert.Equal(golden, encoded);
        Assert.Equal((byte)ServiceWireConstants.Command.ReplyRelayAck, encoded[3]);
        Assert.True(ZLinkServiceWireCodec.TryDecodeReplyRelayAck(
            encoded, out var actual, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(expected, actual);

        var invalid = encoded.ToArray();
        invalid[^1] = 0;
        Assert.False(ZLinkServiceWireCodec.TryDecodeReplyRelayAck(
            invalid, out _, out var invalidStatus));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.InvalidField, invalidStatus);
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeReplyRelayAck(expected with { Status = 0 }));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeReplyRelayAck(
                expected with { ReplyRouteId = 0 }));
    }

    private static byte[] ReadGolden(string name)
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var fixturePath = Path.GetFullPath(
            "../../runtime/protocol/golden/reply-relay-v1.json",
            frameworkRoot);
        using var document = JsonDocument.Parse(File.ReadAllText(fixturePath));
        var fixture = document.RootElement.GetProperty("canonical")
            .EnumerateArray()
            .Single(item => item.GetProperty("name").GetString() == name);
        return Convert.FromHexString(fixture.GetProperty("hex").GetString()!);
    }

    private delegate bool TryDecode<T>(ReadOnlySpan<byte> bytes, out T record,
        out ZLinkServiceWireCodec.DecodeError error);

    private static void AssertGoldenRoundTrip<T>(string name, byte[] encoded,
        TryDecode<T> decode, Func<T, byte[]> encode,
        ZLinkServiceWireCodec.DecodeError trailingError =
            ZLinkServiceWireCodec.DecodeError.TrailingByte)
    {
        var golden = ReadRelocationControlGolden(name);
        Assert.Equal(golden, encoded);
        Assert.True(decode(encoded, out var decoded, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(golden, encode(decoded));
        Assert.False(decode(encoded[..^1], out _, out var truncated));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TruncatedField, truncated);
        Assert.False(decode([.. encoded, 0], out _, out var trailing));
        Assert.Equal(trailingError, trailing);
    }

    private static ZLinkServiceWireCodec.RelocationDataRecord Data(byte phase)
        => new(
            new ZLinkServiceWireCodec.RelocationWireId(4, 5), 6, Coordinator(),
            1, 1, 1,
            ZLinkServiceWireCodec.EncodeFrozenRelocationControl(
              new ZLinkServiceWireCodec.FrozenRelocationControlRecord(
                new ZLinkServiceWireCodec.RequestSourceFence(
                    "source-owner", 9, RoutingId.From("node-a"), 11),
                new MeshOperationId(0, 0), phase, 1,
                new ZLinkServiceWireCodec.RelocationWireId(4, 5), Object(), 0,
                ServiceWireConstants.FrameworkErrorCode.None)));

    private static ZLinkServiceWireCodec.RelocationCoordinatorFence Coordinator()
        => new("coordinator", 7, RoutingId.From("node-a"), 11, "store-3");

    private static ZLinkServiceWireCodec.RelocationCandidateRecord Candidate()
        => new(RoutingId.From("node-b"), 12, "target-owner", 8);

    private static ZLinkServiceWireCodec.RelocationObjectRecord Object()
        => new(2, string.Empty, "spot-1", 9, 10);

    private static IReadOnlyList<ZLinkServiceWireCodec.RelocationParticipantRecord>
        Participants()
        =>
        [
            new(1, 1, default, 0, null, 0, default, 0, 0, 2, 128),
            new(2, 1, default, 0, null, 0, default, 0, 0, 0, 0)
        ];

    private static IReadOnlyList<ZLinkServiceWireCodec.RelocationParticipantRecord>
        BoundSessionJournalParticipants()
        =>
        [
            new(1, 1, default, 0, null, 0, default, 0, 0, 2, 128),
            new(2, 1, default, 0, null, 0, default, 0, 0, 0, 0),
            new(3, 1, default, 0, null, 0, default, 0, 0, 0, 0),
            new(4, 2, RoutingId.From("session-owner"), 5, "session-runtime", 6,
                RoutingId.From("session-a"), 7, 9, 1, 64),
            new(5, 2, RoutingId.From("session-owner"), 5, "session-runtime", 6,
                RoutingId.From("session-b"), 8, 0, 0, 0)
        ];

    private static void AssertBoundSessionJournalParticipants(
        IReadOnlyList<ZLinkServiceWireCodec.RelocationParticipantRecord> participants)
    {
        Assert.Equal(5, participants.Count);
        var journal = participants[3];
        Assert.Equal(4UL, journal.ParticipantId);
        Assert.Equal(2, journal.Kind);
        Assert.Equal(RoutingId.From("session-owner"), journal.SessionOwnerNodeRid);
        Assert.Equal(5UL, journal.SessionOwnerNodeGeneration);
        Assert.Equal("session-runtime", journal.SessionOwnerId);
        Assert.Equal(6UL, journal.SessionOwnerLeaseGeneration);
        Assert.Equal(RoutingId.From("session-a"), journal.SessionRid);
        Assert.Equal(7UL, journal.BindingGeneration);
        Assert.Equal(9UL, journal.LastAcceptedSessionSequence);
        var replacement = participants[4];
        Assert.Equal(5UL, replacement.ParticipantId);
        Assert.Equal(2, replacement.Kind);
        Assert.Equal(RoutingId.From("session-owner"), replacement.SessionOwnerNodeRid);
        Assert.Equal(5UL, replacement.SessionOwnerNodeGeneration);
        Assert.Equal("session-runtime", replacement.SessionOwnerId);
        Assert.Equal(6UL, replacement.SessionOwnerLeaseGeneration);
        Assert.Equal(RoutingId.From("session-b"), replacement.SessionRid);
        Assert.Equal(8UL, replacement.BindingGeneration);
        Assert.Equal(0UL, replacement.LastAcceptedSessionSequence);
    }

    private static byte[] ReadRelocationControlGolden(string name)
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var fixturePath = Path.GetFullPath(
            "../../runtime/protocol/golden/relocation-control-v1.json",
            frameworkRoot);
        using var document = JsonDocument.Parse(File.ReadAllText(fixturePath));
        var fixture = document.RootElement.GetProperty("canonical")
            .EnumerateArray()
            .Single(item => item.GetProperty("name").GetString() == name);
        return Convert.FromHexString(fixture.GetProperty("hex").GetString()!);
    }

    private static byte[] ReadCanonicalRelocationGolden()
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var fixturePath = Path.GetFullPath(
            "../../runtime/protocol/golden/relocation-envelope-v1.json",
            frameworkRoot);
        using var document = JsonDocument.Parse(File.ReadAllText(fixturePath));
        return Convert.FromHexString(
            document.RootElement.GetProperty("logicalHex").GetString()!);
    }
}
