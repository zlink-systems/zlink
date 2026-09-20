using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class GeneratedServiceWireCommandCodecConformanceTests
{
    private static readonly ServiceWireCodec.DecodeContext Context =
        ServiceWireCodec.DecodeContext.Empty;

    [Fact]
    public void Generated_codec_conforms_to_every_indexed_vector()
    {
        using var index = ReadJson("generated/fixtures/index.json");
        Assert.Equal(2, index.RootElement.GetProperty("version").GetInt32());
        Assert.Equal(9, index.RootElement.GetProperty("fixtures").GetArrayLength());

        var cases = new List<ConformanceCase>();
        foreach (var indexed in index.RootElement.GetProperty("fixtures").EnumerateArray())
        {
            using var fixture = ReadJson(indexed.GetProperty("goldenFixture").GetString()!);
            var fixtureRoot = fixture.RootElement.Clone();
            var surface = indexed.GetProperty("surface").Clone();
            var kind = indexed.GetProperty("kind").GetString()!;

            foreach (var canonical in indexed.GetProperty("canonical").EnumerateArray())
            {
                var vector = canonical.Clone();
                var label = $"canonical:{surface.GetProperty("format").GetString()}:"
                    + vector.GetProperty("name").GetString();
                cases.Add(new(label, "accept", () => AssertCanonical(
                    kind, surface, fixtureRoot, vector, label)));
            }

            foreach (var malformed in indexed.GetProperty("malformed").EnumerateArray())
            {
                var vector = malformed.Clone();
                var label = $"malformed:{surface.GetProperty("format").GetString()}:"
                    + vector.GetProperty("name").GetString();
                cases.Add(new(label, "reject", () => DecodeMalformed(
                    surface, fixtureRoot, vector)));
            }
        }

        foreach (var operationCase in index.RootElement.GetProperty("operationCases").EnumerateArray())
        {
            var vector = operationCase.Clone();
            var label = $"operation:{vector.GetProperty("operation").GetString()}:"
                + vector.GetProperty("name").GetString();
            cases.Add(new(label, vector.GetProperty("expect").GetString()!,
                () => ExerciseOperation(vector)));
        }

        Assert.Equal(35, cases.Count);
        foreach (var testCase in cases)
        {
            Assert.True(testCase.Expect is "accept" or "reject", testCase.Label);
            var exception = Record.Exception(testCase.Exercise);
            Assert.False(exception is ConformanceHarnessException,
                $"{testCase.Label}: {exception}");
            Assert.True(testCase.Expect == "reject" ? exception is not null : exception is null,
                $"{testCase.Label}: {exception}");
        }
    }

    private static void AssertCanonical(string kind, JsonElement surface,
        JsonElement fixture, JsonElement vector, string label)
    {
        if (kind == "command")
        {
            var frames = Frames(fixture, vector);
            Assert.Equal(frames, GeneratedCommandRoundTrip(
                surface.GetProperty("commandId").GetInt32(), frames));
            Assert.Equal(frames, PilotCommandRoundTrip(
                surface.GetProperty("commandId").GetInt32(), frames));
            return;
        }

        var property = kind == "logical" ? "logicalHex" : "encodedHex";
        var bytes = Convert.FromHexString(Pointer(fixture,
            vector.GetProperty("pointers").GetProperty(property)).GetString()!);
        Assert.Equal(bytes, GeneratedFormatRoundTrip(
            surface.GetProperty("format").GetString()!, bytes));

        var oracle = PilotFormatRoundTrip(surface.GetProperty("format").GetString()!, bytes);
        if (oracle is not null)
            Assert.True(bytes.SequenceEqual(oracle), $"{label}:oracle");
    }

    private static void DecodeMalformed(JsonElement surface, JsonElement fixture,
        JsonElement vector)
    {
        var commandId = surface.GetProperty("commandId").GetInt32();
        var frames = Frames(fixture, vector);
        var generatedRejected = Record.Exception(
            () => GeneratedCommandRoundTrip(commandId, frames)) is not null;
        var pilotRejected = Record.Exception(
            () => PilotCommandRoundTrip(commandId, frames)) is not null;
        if (generatedRejected && pilotRejected)
            throw new InvalidDataException("generated and pilot codecs rejected the vector");
    }

    private static void ExerciseOperation(JsonElement vector)
    {
        var name = vector.GetProperty("name").GetString();
        if (name == "terminal-predicate")
        {
            var input = vector.GetProperty("input");
            ServiceWireCodec.ValidateTerminalFailure(
                Enum.Parse<ServiceWireCodec.RequestTerminalResult>(
                    Pascal(input.GetProperty("terminalResult").GetString()!)),
                Enum.Parse<ServiceWireCodec.FrameworkErrorCode>(
                    Pascal(input.GetProperty("failureCode").GetString()!)));
            return;
        }

        var bytes = Convert.FromHexString(vector.TryGetProperty("hex", out var hex)
            ? hex.GetString()!
            : vector.GetProperty("framesHex")[0].GetString()!);
        switch (name)
        {
            case "vector-ordering":
                ServiceWireCodec.DecodeLogicalRelocationEnvelopeV1(bytes, Context);
                break;
            case "tlv-unknown-non-empty-skip":
            case "tlv-required-field-presence":
            case "tlv-unknown-with-missing-required":
                ServiceWireCodec.DecodeDescriptorExtension(bytes, Context);
                break;
            case "invalid-utf8":
            case "nul-text":
                ServiceWireCodec.DecodeText8(bytes, Context);
                break;
            case "flag-implication":
                ServiceWireCodec.DecodeActorSend24([bytes], Context);
                break;
            case "metadata-frame-required":
                ServiceWireCodec.DecodeNodeSend16(
                    vector.GetProperty("framesHex").EnumerateArray()
                        .Select(item => Convert.FromHexString(item.GetString()!)).ToArray(), Context);
                break;
            case "durable-flags":
            case "durable-checksum":
            case "durable-trailing":
                ServiceWireCodec.DecodeDurableAuthorityPayloadV1(bytes, Context);
                break;
            default:
                throw new ConformanceHarnessException($"unhandled operation case {name}");
        }
    }

    private static byte[][] GeneratedCommandRoundTrip(int commandId, byte[][] frames) =>
        commandId switch
        {
            28 => ServiceWireCodec.EncodeActorJoin28(
                ServiceWireCodec.DecodeActorJoin28(frames, Context), Context),
            47 => ServiceWireCodec.EncodeUserSpotCreate47(
                ServiceWireCodec.DecodeUserSpotCreate47(frames, Context), Context),
            48 => ServiceWireCodec.EncodeUserSpotClose48(
                ServiceWireCodec.DecodeUserSpotClose48(frames, Context), Context),
            49 => ServiceWireCodec.EncodeActorCreate49(
                ServiceWireCodec.DecodeActorCreate49(frames, Context), Context),
            _ => throw new InvalidDataException($"unhandled command {commandId}")
        };

    private static byte[][] PilotCommandRoundTrip(int commandId, byte[][] frames) =>
        commandId switch
        {
            28 => ServiceWirePilotCodec.EncodeActorJoin28(
                ServiceWirePilotCodec.DecodeActorJoin28(frames)),
            47 => [ServiceWirePilotCodec.EncodeUserSpotCreate47(
                ServiceWirePilotCodec.DecodeUserSpotCreate47(frames.Single()))],
            48 => [ServiceWirePilotCodec.EncodeUserSpotClose48(
                ServiceWirePilotCodec.DecodeUserSpotClose48(frames.Single()))],
            49 => [ServiceWirePilotCodec.EncodeActorCreate49(
                ServiceWirePilotCodec.DecodeActorCreate49(frames.Single()))],
            _ => throw new InvalidDataException($"unhandled command oracle {commandId}")
        };

    private static byte[] GeneratedFormatRoundTrip(string format, byte[] bytes) => format switch
    {
        "authority-payload-v1" => ServiceWireCodec.EncodeDurableAuthorityPayloadV1(
            ServiceWireCodec.DecodeDurableAuthorityPayloadV1(bytes, Context), Context),
        "instance-activation-recovery-v1" =>
            ServiceWireCodec.EncodeDurableInstanceActivationRecoveryV1(
                ServiceWireCodec.DecodeDurableInstanceActivationRecoveryV1(bytes, Context), Context),
        "relocation-data-chunk-v1" => ServiceWireCodec.EncodeDurableRelocationDataChunkV1(
            ServiceWireCodec.DecodeDurableRelocationDataChunkV1(bytes, Context), Context),
        "relocation-manifest-v1" => ServiceWireCodec.EncodeDurableRelocationManifestV1(
            ServiceWireCodec.DecodeDurableRelocationManifestV1(bytes, Context), Context),
        "relocation-envelope-v1" => ServiceWireCodec.EncodeLogicalRelocationEnvelopeV1(
            ServiceWireCodec.DecodeLogicalRelocationEnvelopeV1(bytes, Context), Context),
        _ => throw new InvalidDataException($"unhandled format {format}")
    };

    private static byte[]? PilotFormatRoundTrip(string format, byte[] bytes) => format switch
    {
        "relocation-data-chunk-v1" => ServiceWirePilotCodec.EncodeRelocationDataChunkV1(
            ServiceWirePilotCodec.DecodeRelocationDataChunkV1(bytes)),
        "relocation-manifest-v1" => ServiceWirePilotCodec.EncodeRelocationManifestV1(
            ServiceWirePilotCodec.DecodeRelocationManifestV1(bytes)),
        "relocation-envelope-v1" => ServiceWirePilotCodec.EncodeRelocationEnvelopeV1(
            ServiceWirePilotCodec.DecodeRelocationEnvelopeV1(bytes)),
        _ => null
    };

    private static byte[][] Frames(JsonElement fixture, JsonElement vector)
    {
        var pointers = vector.GetProperty("pointers");
        var pointer = pointers.TryGetProperty("framesHex", out var framesHex)
            ? framesHex
            : pointers.GetProperty("hex");
        var value = Pointer(fixture, pointer);
        return value.ValueKind == JsonValueKind.Array
            ? value.EnumerateArray().Select(item => Convert.FromHexString(item.GetString()!)).ToArray()
            : [Convert.FromHexString(value.GetString()!)];
    }

    private static JsonElement Pointer(JsonElement root, JsonElement pointer)
    {
        var current = root;
        foreach (var segment in pointer.GetString()!.Split('/').Skip(1))
        {
            var decoded = segment.Replace("~1", "/").Replace("~0", "~");
            current = current.ValueKind == JsonValueKind.Array
                ? current[int.Parse(decoded)]
                : current.GetProperty(decoded);
        }
        return current;
    }

    private static string Pascal(string value) => char.ToUpperInvariant(value[0]) + value[1..];

    private static JsonDocument ReadJson(string relativePath)
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var path = Path.GetFullPath($"../../runtime/protocol/{relativePath}", frameworkRoot);
        return JsonDocument.Parse(File.ReadAllText(path));
    }

    private sealed record ConformanceCase(string Label, string Expect, Action Exercise);

    private sealed class ConformanceHarnessException(string message) : Exception(message);
}
