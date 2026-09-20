using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class GeneratedServiceWireCommandCodecConformanceTests
{
    [Fact]
    public void Fixture_catalog_has_the_nine_renderer_surfaces()
    {
        using var index = ReadJson("generated/fixtures/index.json");

        Assert.Equal(9, index.RootElement.GetProperty("fixtures").GetArrayLength());
    }

    [Theory]
    [InlineData("authority")]
    [InlineData("activation")]
    [InlineData("chunk")]
    [InlineData("manifest")]
    [InlineData("logical")]
    public void Durable_and_logical_generated_codecs_round_trip_canonical_bytes(string surface)
    {
        switch (surface)
        {
            case "authority":
                var authority = ReadHex("durable-authority-v1.json", "encodedHex");
                Assert.Equal(authority, ServiceWireCodec.EncodeDurableAuthorityPayloadV1(
                    ServiceWireCodec.DecodeDurableAuthorityPayloadV1(authority)));
                break;
            case "activation":
                var activation = ReadHex("instance-activation-recovery-v1.json", "encodedHex");
                Assert.Equal(activation,
                    ServiceWireCodec.EncodeDurableInstanceActivationRecoveryV1(
                        ServiceWireCodec.DecodeDurableInstanceActivationRecoveryV1(activation)));
                break;
            case "chunk":
                var chunk = ReadHex("relocation-data-chunk-v1.json", "encodedHex");
                Assert.Equal(chunk, ServiceWireCodec.EncodeDurableRelocationDataChunkV1(
                    ServiceWireCodec.DecodeDurableRelocationDataChunkV1(chunk)));
                Assert.Equal(chunk, ServiceWirePilotCodec.EncodeRelocationDataChunkV1(
                    ServiceWirePilotCodec.DecodeRelocationDataChunkV1(chunk)));
                break;
            case "manifest":
                var manifest = ReadHex("relocation-manifest-v1.json", "encodedHex");
                Assert.Equal(manifest, ServiceWireCodec.EncodeDurableRelocationManifestV1(
                    ServiceWireCodec.DecodeDurableRelocationManifestV1(manifest)));
                Assert.Equal(manifest, ServiceWirePilotCodec.EncodeRelocationManifestV1(
                    ServiceWirePilotCodec.DecodeRelocationManifestV1(manifest)));
                break;
            case "logical":
                var logical = ReadHex("relocation-envelope-v1.json", "logicalHex");
                Assert.Equal(logical, ServiceWireCodec.EncodeLogicalRelocationEnvelopeV1(
                    ServiceWireCodec.DecodeLogicalRelocationEnvelopeV1(logical)));
                Assert.Equal(logical, ServiceWirePilotCodec.EncodeRelocationEnvelopeV1(
                    ServiceWirePilotCodec.DecodeRelocationEnvelopeV1(logical)));
                break;
        }
    }

    [Fact]
    public void Actor_join_generated_and_pilot_codecs_match_canonical_and_malformed_vectors()
    {
        using var fixture = ReadJson("golden/actor-join-request-v1.json");
        foreach (var vector in fixture.RootElement.GetProperty("valid").EnumerateArray())
        {
            var frames = ReadFrames(vector, "framesHex");
            Assert.Equal(frames, ServiceWireCodec.EncodeActorJoin28(
                ServiceWireCodec.DecodeActorJoin28(frames)));
            Assert.Equal(frames, ServiceWirePilotCodec.EncodeActorJoin28(
                ServiceWirePilotCodec.DecodeActorJoin28(frames)));
        }

        foreach (var vector in fixture.RootElement.GetProperty("invalid").EnumerateArray())
        {
            var frames = ReadFrames(vector, "framesHex");
            Assert.ThrowsAny<Exception>(() => ServiceWireCodec.DecodeActorJoin28(frames));
            Assert.ThrowsAny<Exception>(() => ServiceWirePilotCodec.DecodeActorJoin28(frames));
        }
    }

    [Theory]
    [InlineData("user-spot-create-v1.json", 47)]
    [InlineData("user-spot-close-v1.json", 48)]
    [InlineData("actor-create-v1.json", 49)]
    public void Creation_command_generated_and_pilot_codecs_match_canonical_and_malformed_vectors(
        string file, int commandId)
    {
        using var fixture = ReadJson($"golden/{file}");
        var canonical = Convert.FromHexString(fixture.RootElement
            .GetProperty("canonical").GetProperty("hex").GetString()!);

        Assert.Equal(canonical, GeneratedRoundTrip(commandId, canonical));
        Assert.Equal(canonical, PilotRoundTrip(commandId, canonical));

        foreach (var malformed in fixture.RootElement.GetProperty("malformed").EnumerateArray())
        {
            var bytes = Convert.FromHexString(malformed.GetProperty("hex").GetString()!);
            Assert.ThrowsAny<Exception>(() => GeneratedRoundTrip(commandId, bytes));
            Assert.ThrowsAny<Exception>(() => PilotRoundTrip(commandId, bytes));
        }
    }

    private static byte[] GeneratedRoundTrip(int commandId, byte[] bytes) => commandId switch
    {
        47 => ServiceWireCodec.EncodeUserSpotCreate47(
            ServiceWireCodec.DecodeUserSpotCreate47(bytes)).Single(),
        48 => ServiceWireCodec.EncodeUserSpotClose48(
            ServiceWireCodec.DecodeUserSpotClose48(bytes)).Single(),
        49 => ServiceWireCodec.EncodeActorCreate49(
            ServiceWireCodec.DecodeActorCreate49(bytes)).Single(),
        _ => throw new InvalidOperationException()
    };

    private static byte[] PilotRoundTrip(int commandId, byte[] bytes) => commandId switch
    {
        47 => ServiceWirePilotCodec.EncodeUserSpotCreate47(
            ServiceWirePilotCodec.DecodeUserSpotCreate47(bytes)),
        48 => ServiceWirePilotCodec.EncodeUserSpotClose48(
            ServiceWirePilotCodec.DecodeUserSpotClose48(bytes)),
        49 => ServiceWirePilotCodec.EncodeActorCreate49(
            ServiceWirePilotCodec.DecodeActorCreate49(bytes)),
        _ => throw new InvalidOperationException()
    };

    private static byte[] ReadHex(string file, string property)
    {
        using var fixture = ReadJson($"golden/{file}");
        return Convert.FromHexString(fixture.RootElement.GetProperty(property).GetString()!);
    }

    private static byte[][] ReadFrames(JsonElement value, string property) =>
        value.GetProperty(property).EnumerateArray()
            .Select(static frame => Convert.FromHexString(frame.GetString()!))
            .ToArray();

    private static JsonDocument ReadJson(string relativePath)
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var path = Path.GetFullPath($"../../runtime/protocol/{relativePath}", frameworkRoot);
        return JsonDocument.Parse(File.ReadAllText(path));
    }
}
