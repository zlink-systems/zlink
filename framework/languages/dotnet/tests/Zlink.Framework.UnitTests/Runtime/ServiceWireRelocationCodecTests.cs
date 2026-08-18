using System.Reflection;
using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests;

public sealed class ServiceWireRelocationCodecTests
{
    [Fact]
    public void Commands_30_31_34_and_40_match_the_shared_golden_vectors()
    {
        var relocation = new ZLinkServiceWireCodec.RelocationWireId(4, 5);
        var coordinator = Coordinator();
        var target = Target();
        var objectRecord = Object();

        AssertGoldenRoundTrip(
            "relocationReady",
            new ZLinkServiceWireCodec.RelocationReadyRecord(
                relocation, 6, coordinator, target, objectRecord, 2),
            ZLinkServiceWireCodec.EncodeRelocationReady,
            ZLinkServiceWireCodec.TryDecodeRelocationReady);

        var data = AssertGoldenDecodeRoundTrip<
            ZLinkServiceWireCodec.RelocationDataRecord>(
            "relocationDataPostCaptureIngress",
            ZLinkServiceWireCodec.TryDecodeRelocationData,
            ZLinkServiceWireCodec.EncodeRelocationData,
            ZLinkServiceWireCodec.DecodeError.InvalidField);
        Assert.Equal(relocation, data.RelocationId);
        Assert.Equal(6UL, data.TargetAttemptGeneration);
        Assert.Equal(coordinator, data.Coordinator);
        Assert.Equal(1, data.SenderRole);
        Assert.Equal(objectRecord, data.Object);
        Assert.Equal(6, data.FrozenRecord.Encoded.Span[0]);

        AssertGoldenRoundTrip(
            "relocationCutover",
            new ZLinkServiceWireCodec.RelocationCutoverRecord(
                relocation, 6, coordinator, 1, objectRecord, 3, 0xE3069283),
            ZLinkServiceWireCodec.EncodeRelocationCutover,
            ZLinkServiceWireCodec.TryDecodeRelocationCutover);

        AssertGoldenRoundTrip(
            "relocationPrepareRestore",
            new ZLinkServiceWireCodec.RelocationPrepareRecord(
                relocation,
                6,
                coordinator,
                target,
                1,
                objectRecord,
                RoutingId.From("node-a"),
                11,
                24,
                2,
                0x29BC8795,
                1),
            ZLinkServiceWireCodec.EncodeRelocationPrepare,
            ZLinkServiceWireCodec.TryDecodeRelocationPrepare);
    }

    [Fact]
    public void Command_52_relocation_state_matches_the_shared_golden_vectors()
    {
        var relocation = new ZLinkServiceWireCodec.RelocationWireId(4, 5);
        var coordinator = Coordinator();
        var objectRecord = Object();

        foreach (var (name, ordinal, dataLength, firstByte) in new[]
                 {
                     ("relocationStateChunk", 0u, 16, (byte)0x00),
                     ("relocationStateFinalChunk", 1u, 8, (byte)0x10)
                 })
        {
            var golden = ReadRelocationControlGolden(name);
            Assert.True(ZLinkServiceWireCodec.TryDecodeRelocationState(
                golden, out var decoded, out var error));
            Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
            Assert.Equal(relocation, decoded.RelocationId);
            Assert.Equal(6UL, decoded.TargetAttemptGeneration);
            Assert.Equal(coordinator, decoded.Coordinator);
            Assert.Equal(1, decoded.SenderRole);
            Assert.Equal(objectRecord, decoded.Object);
            Assert.Equal(ordinal, decoded.ChunkOrdinal);
            Assert.Equal(dataLength, decoded.ChunkData.Length);
            Assert.Equal(firstByte, decoded.ChunkData.Span[0]);
            Assert.Equal(golden,
                ZLinkServiceWireCodec.EncodeRelocationState(decoded));
            Assert.False(ZLinkServiceWireCodec.TryDecodeRelocationState(
                golden[..^1], out _, out var truncated));
            Assert.Equal(ZLinkServiceWireCodec.DecodeError.TruncatedField,
                truncated);
            Assert.False(ZLinkServiceWireCodec.TryDecodeRelocationState(
                [.. golden, 0], out _, out var trailing));
            Assert.Equal(ZLinkServiceWireCodec.DecodeError.TrailingByte,
                trailing);
        }

        foreach (var (name, expected) in new[]
                 {
                     ("relocationStateZeroTargetAttemptGeneration",
                         ZLinkServiceWireCodec.DecodeError.InvalidField),
                     ("relocationStateTruncatedChunkData",
                         ZLinkServiceWireCodec.DecodeError.TruncatedField),
                     ("relocationStateTrailingByte",
                         ZLinkServiceWireCodec.DecodeError.TrailingByte)
                 })
        {
            var malformed = ReadRelocationControlMalformed(name);
            Assert.False(ZLinkServiceWireCodec.TryDecodeRelocationState(
                malformed, out _, out var error));
            Assert.Equal(expected, error);
        }
    }

    [Fact]
    public void Relocation_command_DTOs_expose_only_the_canonical_fields()
    {
        Assert.Equal(
            new[]
            {
                "RelocationId",
                "TargetAttemptGeneration",
                "Coordinator",
                "Target",
                "Object",
                "SenderRole"
            },
            PublicPropertyNames<ZLinkServiceWireCodec.RelocationReadyRecord>());
        Assert.Equal(
            new[]
            {
                "RelocationId",
                "TargetAttemptGeneration",
                "Coordinator",
                "SenderRole",
                "Object",
                "FrozenRecord"
            },
            PublicPropertyNames<ZLinkServiceWireCodec.RelocationDataRecord>());
        Assert.Equal(
            new[]
            {
                "RelocationId",
                "TargetAttemptGeneration",
                "Coordinator",
                "SenderRole",
                "Object",
                "BoundaryRecordCount",
                "BoundaryChecksumCrc32c"
            },
            PublicPropertyNames<ZLinkServiceWireCodec.RelocationCutoverRecord>());
        Assert.Equal(
            new[]
            {
                "RelocationId",
                "TargetAttemptGeneration",
                "Coordinator",
                "SenderRole",
                "Object",
                "ChunkOrdinal",
                "ChunkData"
            },
            PublicPropertyNames<ZLinkServiceWireCodec.RelocationStateRecord>());
        Assert.Equal(
            new[]
            {
                "RelocationId",
                "TargetAttemptGeneration",
                "Coordinator",
                "Target",
                "InitiatorRole",
                "Object",
                "SourceNodeRid",
                "SourceNodeGeneration",
                "PayloadTotalLength",
                "PayloadChunkCount",
                "PayloadChecksumCrc32c",
                "ApplicationVersion"
            },
            PublicPropertyNames<ZLinkServiceWireCodec.RelocationPrepareRecord>());

        var methodNames = typeof(ZLinkServiceWireCodec)
            .GetMethods(BindingFlags.Static | BindingFlags.NonPublic)
            .Select(static method => method.Name)
            .ToHashSet(StringComparer.Ordinal);
        Assert.DoesNotContain("EncodeRelocationAck", methodNames);
        Assert.DoesNotContain("TryDecodeRelocationAck", methodNames);
        Assert.DoesNotContain("EncodeRelocationComplete", methodNames);
        Assert.DoesNotContain("TryDecodeRelocationComplete", methodNames);
        Assert.DoesNotContain("EncodeRelocationReserved", methodNames);
        Assert.DoesNotContain("TryDecodeRelocationReserved", methodNames);
        Assert.DoesNotContain("EncodeRelocationSeal", methodNames);
        Assert.DoesNotContain("TryDecodeRelocationSeal", methodNames);
    }

    [Fact]
    public void Reserved_command_ids_are_not_accepted_as_relocation_ready()
    {
        foreach (var reserved in new byte[] { 32, 35, 41 })
        {
            var encoded = ReadRelocationControlGolden("relocationReady");
            encoded[3] = reserved;
            Assert.False(ZLinkServiceWireCodec.TryDecodeRelocationReady(
                encoded, out _, out var error));
            Assert.Equal(ZLinkServiceWireCodec.DecodeError.UnknownCommand,
                error);
        }
    }

    [Fact]
    public void Relocation_data_rejects_unknown_frozen_record_kinds()
    {
        var record = AssertGoldenDecodeRoundTrip<
            ZLinkServiceWireCodec.RelocationDataRecord>(
            "relocationDataPostCaptureIngress",
            ZLinkServiceWireCodec.TryDecodeRelocationData,
            ZLinkServiceWireCodec.EncodeRelocationData,
            ZLinkServiceWireCodec.DecodeError.InvalidField);
        var invalid = record.FrozenRecord.Encoded.ToArray();
        invalid[0] = 15;

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeRelocationData(record with
            {
                FrozenRecord = new ZLinkServiceWireCodec.FrozenRecord(invalid)
            }));
    }

    [Fact]
    public void Instance_spot_prepare_uses_the_schema_normalized_object_shape()
    {
        var prepare = new ZLinkServiceWireCodec.RelocationPrepareRecord(
            new ZLinkServiceWireCodec.RelocationWireId(4, 5),
            6,
            Coordinator(),
            Target(),
            1,
            new ZLinkServiceWireCodec.RelocationObjectRecord(
                3, "Game.Instance", "instance-1", 9, 0),
            RoutingId.From("node-a"),
            11,
            0,
            0,
            0,
            0);

        var encoded = ZLinkServiceWireCodec.EncodeRelocationPrepare(prepare);
        Assert.True(ZLinkServiceWireCodec.TryDecodeRelocationPrepare(
            encoded, out var decoded, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(prepare, decoded);
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeRelocationPrepare(prepare with
            {
                Object = prepare.Object with
                {
                    ExpectedAuthorityOwnerGeneration = 10
                }
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
        var golden = ReadReplyRelayGolden("maintenanceReplyRelay");

        Assert.Equal(golden, encoded);
        Assert.True(ZLinkServiceWireCodec.TryDecodeReplyRelay(
            encoded, out var actual, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(expected, actual);
        Assert.False(ZLinkServiceWireCodec.TryDecodeReplyRelay(
            encoded[..^1], out _, out var truncated));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TruncatedField,
            truncated);
        Assert.False(ZLinkServiceWireCodec.TryDecodeReplyRelay(
            [.. encoded, 0], out _, out var trailing));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TrailingByte, trailing);
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
        Assert.Equal(ReadReplyRelayGolden("replyRelayAlreadyTerminalAck"),
            encoded);
        Assert.True(ZLinkServiceWireCodec.TryDecodeReplyRelayAck(
            encoded, out var actual, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(expected, actual);

        var invalid = encoded.ToArray();
        invalid[^1] = 0;
        Assert.False(ZLinkServiceWireCodec.TryDecodeReplyRelayAck(
            invalid, out _, out var invalidStatus));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.InvalidField,
            invalidStatus);
    }

    private delegate bool TryDecode<T>(ReadOnlySpan<byte> bytes, out T record,
        out ZLinkServiceWireCodec.DecodeError error);

    private static void AssertGoldenRoundTrip<T>(string name, T value,
        Func<T, byte[]> encode, TryDecode<T> decode)
    {
        var encoded = encode(value);
        Assert.Equal(ReadRelocationControlGolden(name), encoded);
        Assert.True(decode(encoded, out var decoded, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(value, decoded);
        Assert.Equal(encoded, encode(decoded));
        Assert.False(decode(encoded[..^1], out _, out var truncated));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TruncatedField,
            truncated);
        Assert.False(decode([.. encoded, 0], out _, out var trailing));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TrailingByte, trailing);

        var forbiddenFlag = encoded.ToArray();
        forbiddenFlag[4] = 1;
        Assert.False(decode(forbiddenFlag, out _, out var flagError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.ForbiddenFlag,
            flagError);
    }

    private static T AssertGoldenDecodeRoundTrip<T>(string name,
        TryDecode<T> decode, Func<T, byte[]> encode,
        ZLinkServiceWireCodec.DecodeError trailingError)
    {
        var encoded = ReadRelocationControlGolden(name);
        Assert.True(decode(encoded, out var decoded, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(encoded, encode(decoded));
        Assert.False(decode(encoded[..^1], out _, out var truncated));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TruncatedField,
            truncated);
        Assert.False(decode([.. encoded, 0], out _, out var trailing));
        Assert.Equal(trailingError, trailing);
        return decoded;
    }

    private static string[] PublicPropertyNames<T>() =>
        typeof(T).GetProperties(BindingFlags.Instance | BindingFlags.Public)
            .Select(static property => property.Name)
            .ToArray();

    private static ZLinkServiceWireCodec.RelocationCoordinatorFence Coordinator()
        => new("coordinator", 7, RoutingId.From("node-a"), 11, "store-3");

    private static ZLinkServiceWireCodec.RelocationTargetRecord Target()
        => new(RoutingId.From("node-b"), 12, "target-owner", 8);

    private static ZLinkServiceWireCodec.RelocationObjectRecord Object()
        => new(2, string.Empty, "spot-1", 9, 10);

    private static byte[] ReadReplyRelayGolden(string name) =>
        ReadGolden("reply-relay-v1.json", name);

    private static byte[] ReadRelocationControlGolden(string name) =>
        ReadGolden("relocation-control-v1.json", "canonical", name);

    private static byte[] ReadRelocationControlMalformed(string name) =>
        ReadGolden("relocation-control-v1.json", "malformed", name);

    private static byte[] ReadGolden(string file, string name) =>
        ReadGolden(file, "canonical", name);

    private static byte[] ReadGolden(string file, string section, string name)
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var fixturePath = Path.GetFullPath(
            $"../../runtime/protocol/golden/{file}", frameworkRoot);
        using var document = JsonDocument.Parse(File.ReadAllText(fixturePath));
        var fixture = document.RootElement.GetProperty(section)
            .EnumerateArray()
            .Single(item => item.GetProperty("name").GetString() == name);
        return Convert.FromHexString(fixture.GetProperty("hex").GetString()!);
    }
}
