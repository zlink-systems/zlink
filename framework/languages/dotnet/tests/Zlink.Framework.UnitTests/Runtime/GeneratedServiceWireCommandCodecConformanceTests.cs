using System.Text;
using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Actors;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class GeneratedServiceWireCommandCodecConformanceTests
{
    [Fact]
    public void Batch3_runtime_and_generated_codecs_match_canonical_goldens()
    {
        foreach (var bytes in ReadArray("reply-relay-v1.json", "canonical")
                     .Concat(ReadArray("relocation-control-v1.json", "canonical"))
                     .Concat(ReadArray("session-relocation-barrier-v1.json", "canonical")))
        {
            Assert.Equal(bytes, RuntimeCommandRoundTrip(bytes));
            Assert.Equal(bytes, GeneratedCommandRoundTrip(bytes));
        }
    }

    [Fact]
    public void Batch3_runtime_and_generated_codecs_reject_the_same_malformed_bytes()
    {
        var malformed = ReadArray("relocation-control-v1.json", "malformed")
            .Concat(ReadArray("reply-relay-v1.json", "canonical")
                .SelectMany(Mutations))
            .Concat(ReadArray("session-relocation-barrier-v1.json", "canonical")
                .SelectMany(Mutations));

        foreach (var bytes in malformed)
        {
            Assert.True(RuntimeCommandRejects(bytes));
            Assert.ThrowsAny<Exception>(() => GeneratedCommandRoundTrip(bytes));
        }
    }

    [Fact]
    public void Batch4_runtime_and_generated_codecs_match_canonical_goldens()
    {
        foreach (var file in new[]
                 {
                     "user-spot-create-v1.json",
                     "user-spot-close-v1.json",
                     "actor-create-v1.json"
                 })
        {
            var bytes = ReadCanonicalObject(file);
            Assert.Equal(bytes, RuntimeCommandRoundTrip(bytes));
            Assert.Equal(bytes, GeneratedCommandRoundTrip(bytes));
        }
    }

    [Fact]
    public void Batch4_runtime_and_generated_codecs_reject_the_same_malformed_goldens()
    {
        foreach (var file in new[]
                 {
                     "user-spot-create-v1.json",
                     "user-spot-close-v1.json",
                     "actor-create-v1.json"
                 })
        foreach (var bytes in ReadArray(file, "malformed"))
        {
            Assert.True(RuntimeCommandRejects(bytes));
            Assert.ThrowsAny<Exception>(() => GeneratedCommandRoundTrip(bytes));
        }
    }

    [Fact]
    public void Zljr_runtime_and_generated_codecs_match_the_canonical_golden()
    {
        var bytes = ReadCanonicalObject("zljr-v1.json");

        Assert.True(ZLinkActorRemoteJoinRecoverySavedWork.TryDecode(
            bytes, out var source, out var recovery));
        var runtime = ZLinkActorRemoteJoinRecoverySavedWork.Create(
            1,
            source,
            ZLinkActorRemoteJoinRecoveryCodec.Encode(recovery));
        var generated = ServiceWirePilotCodec.DecodeZljrRecordV1(bytes);
        var runtimeBytes = runtime.Payload.ToArray();
        var runtimeGenerated = ServiceWirePilotCodec.DecodeZljrRecordV1(
            runtimeBytes);

        Assert.Equal(
            Encoding.UTF8.GetString(generated.Metadata),
            Encoding.UTF8.GetString(runtimeGenerated.Metadata));
        Assert.Equal(bytes, runtimeBytes);
        Assert.Equal(bytes, ServiceWirePilotCodec.EncodeZljrRecordV1(generated));
    }

    [Fact]
    public void Zljr_runtime_and_generated_codecs_reject_the_same_malformed_goldens()
    {
        foreach (var bytes in ReadArray("zljr-v1.json", "malformed"))
        {
            Assert.False(ZLinkActorRemoteJoinRecoverySavedWork.TryDecode(
                bytes, out _, out _));
            Assert.ThrowsAny<Exception>(() =>
                ServiceWirePilotCodec.DecodeZljrRecordV1(bytes));
        }
    }

    private static byte[] RuntimeCommandRoundTrip(byte[] bytes) => bytes[3] switch
    {
        30 => RoundTrip<ZLinkServiceWireCodec.RelocationReadyRecord>(bytes,
            ZLinkServiceWireCodec.TryDecodeRelocationReady,
            ZLinkServiceWireCodec.EncodeRelocationReady),
        31 => RoundTrip<ZLinkServiceWireCodec.RelocationDataRecord>(bytes,
            ZLinkServiceWireCodec.TryDecodeRelocationData,
            ZLinkServiceWireCodec.EncodeRelocationData),
        33 => RoundTrip<ZLinkServiceWireCodec.ReplyRelayRecord>(bytes,
            ZLinkServiceWireCodec.TryDecodeReplyRelay,
            ZLinkServiceWireCodec.EncodeReplyRelay),
        34 => RoundTrip<ZLinkServiceWireCodec.RelocationCutoverRecord>(bytes,
            ZLinkServiceWireCodec.TryDecodeRelocationCutover,
            ZLinkServiceWireCodec.EncodeRelocationCutover),
        40 => RoundTrip<ZLinkServiceWireCodec.RelocationPrepareRecord>(bytes,
            ZLinkServiceWireCodec.TryDecodeRelocationPrepare,
            ZLinkServiceWireCodec.EncodeRelocationPrepare),
        42 => RoundTrip<ZLinkServiceWireCodec.SessionRelocationSealRecord>(bytes,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationSeal,
            ZLinkServiceWireCodec.EncodeSessionRelocationSeal),
        43 => RoundTrip<ZLinkServiceWireCodec.SessionRelocationSealedRecord>(bytes,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationSealed,
            ZLinkServiceWireCodec.EncodeSessionRelocationSealed),
        44 => RoundTrip<ZLinkServiceWireCodec.SessionRelocationRouteRecord>(bytes,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationRoute,
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute),
        46 => RoundTrip<ZLinkServiceWireCodec.ReplyRelayAckRecord>(bytes,
            ZLinkServiceWireCodec.TryDecodeReplyRelayAck,
            ZLinkServiceWireCodec.EncodeReplyRelayAck),
        47 or 48 => RoundTripUserSpot(bytes),
        49 => RoundTripActorCreate(bytes),
        52 => RoundTrip<ZLinkServiceWireCodec.RelocationStateRecord>(bytes,
            ZLinkServiceWireCodec.TryDecodeRelocationState,
            ZLinkServiceWireCodec.EncodeRelocationState),
        53 => RoundTrip<ZLinkServiceWireCodec.RelocationFailedRecord>(bytes,
            ZLinkServiceWireCodec.TryDecodeRelocationFailed,
            ZLinkServiceWireCodec.EncodeRelocationFailed),
        _ => throw new InvalidOperationException()
    };

    private static byte[] GeneratedCommandRoundTrip(byte[] bytes) => bytes[3] switch
    {
        30 => ServiceWirePilotCodec.EncodeRelocationReady30(
            ServiceWirePilotCodec.DecodeRelocationReady30(bytes)),
        31 => ServiceWirePilotCodec.EncodeRelocationData31(
            ServiceWirePilotCodec.DecodeRelocationData31(bytes)),
        33 => ServiceWirePilotCodec.EncodeReplyRelay33(
            ServiceWirePilotCodec.DecodeReplyRelay33([bytes])).Single(),
        34 => ServiceWirePilotCodec.EncodeRelocationCutover34(
            ServiceWirePilotCodec.DecodeRelocationCutover34(bytes)),
        40 => ServiceWirePilotCodec.EncodeRelocationPrepare40(
            ServiceWirePilotCodec.DecodeRelocationPrepare40(bytes)),
        42 => ServiceWirePilotCodec.EncodeSessionRelocationSeal42(
            ServiceWirePilotCodec.DecodeSessionRelocationSeal42(bytes)),
        43 => ServiceWirePilotCodec.EncodeSessionRelocationSealed43(
            ServiceWirePilotCodec.DecodeSessionRelocationSealed43(bytes)),
        44 => ServiceWirePilotCodec.EncodeSessionRelocationRoute44(
            ServiceWirePilotCodec.DecodeSessionRelocationRoute44(bytes)),
        46 => ServiceWirePilotCodec.EncodeReplyRelayAck46(
            ServiceWirePilotCodec.DecodeReplyRelayAck46(bytes)),
        47 => ServiceWirePilotCodec.EncodeUserSpotCreate47(
            ServiceWirePilotCodec.DecodeUserSpotCreate47(bytes)),
        48 => ServiceWirePilotCodec.EncodeUserSpotClose48(
            ServiceWirePilotCodec.DecodeUserSpotClose48(bytes)),
        49 => ServiceWirePilotCodec.EncodeActorCreate49(
            ServiceWirePilotCodec.DecodeActorCreate49(bytes)),
        52 => ServiceWirePilotCodec.EncodeRelocationState52(
            ServiceWirePilotCodec.DecodeRelocationState52(bytes)),
        53 => ServiceWirePilotCodec.EncodeRelocationFailed53(
            ServiceWirePilotCodec.DecodeRelocationFailed53(bytes)),
        _ => throw new InvalidOperationException()
    };

    private static bool RuntimeCommandRejects(byte[] bytes) => bytes[3] switch
    {
        30 => !ZLinkServiceWireCodec.TryDecodeRelocationReady(bytes, out _, out _),
        31 => !ZLinkServiceWireCodec.TryDecodeRelocationData(bytes, out _, out _),
        33 => !ZLinkServiceWireCodec.TryDecodeReplyRelay(bytes, out _, out _),
        34 => !ZLinkServiceWireCodec.TryDecodeRelocationCutover(bytes, out _, out _),
        40 => !ZLinkServiceWireCodec.TryDecodeRelocationPrepare(bytes, out _, out _),
        42 => !ZLinkServiceWireCodec.TryDecodeSessionRelocationSeal(bytes, out _, out _),
        43 => !ZLinkServiceWireCodec.TryDecodeSessionRelocationSealed(bytes, out _, out _),
        44 => !ZLinkServiceWireCodec.TryDecodeSessionRelocationRoute(bytes, out _, out _),
        46 => !ZLinkServiceWireCodec.TryDecodeReplyRelayAck(bytes, out _, out _),
        47 or 48 => !ZLinkServiceWireCodec.TryDecodeUserSpotOperation(bytes, out _, out _),
        49 => !ZLinkServiceWireCodec.TryDecodeActorCreateOperation(bytes, out _, out _),
        52 => !ZLinkServiceWireCodec.TryDecodeRelocationState(bytes, out _, out _),
        53 => !ZLinkServiceWireCodec.TryDecodeRelocationFailed(bytes, out _, out _),
        _ => throw new InvalidOperationException()
    };

    private static byte[] RoundTripUserSpot(byte[] bytes)
    {
        Assert.True(ZLinkServiceWireCodec.TryDecodeUserSpotOperation(
            bytes, out var record, out _));
        return record.Command == ServiceWireConstants.Command.UserSpotCreate
            ? ZLinkServiceWireCodec.EncodeUserSpotCreate(record.Create)
            : ZLinkServiceWireCodec.EncodeUserSpotClose(record.Close);
    }

    private static byte[] RoundTripActorCreate(byte[] bytes)
    {
        Assert.True(ZLinkServiceWireCodec.TryDecodeActorCreateOperation(
            bytes, out var record, out _));
        return ZLinkServiceWireCodec.EncodeActorCreate(record.Operation);
    }

    private delegate bool TryDecode<T>(ReadOnlySpan<byte> bytes, out T value,
        out ZLinkServiceWireCodec.DecodeError error);

    private static byte[] RoundTrip<T>(byte[] bytes, TryDecode<T> decode,
        Func<T, byte[]> encode)
    {
        Assert.True(decode(bytes, out var value, out _));
        return encode(value);
    }

    private static IEnumerable<byte[]> Mutations(byte[] bytes)
    {
        yield return bytes[..^1];
        yield return [.. bytes, 0];
    }

    private static byte[] ReadCanonicalObject(string file) =>
        ReadFixture(file).GetProperty("canonical").GetProperty("hex")
            .GetString() is { } hex
            ? Convert.FromHexString(hex)
            : throw new InvalidDataException();

    private static IReadOnlyList<byte[]> ReadArray(string file, string section) =>
        ReadFixture(file).GetProperty(section).EnumerateArray()
            .Select(static item => Convert.FromHexString(
                item.GetProperty("hex").GetString()!))
            .ToArray();

    private static JsonElement ReadFixture(string file)
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var path = Path.GetFullPath(
            $"../../runtime/protocol/golden/{file}", frameworkRoot);
        using var document = JsonDocument.Parse(File.ReadAllText(path));
        return document.RootElement.Clone();
    }
}
