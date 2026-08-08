using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.UnitTests;

public sealed class FrameworkJsonProfileTests
{
    [Fact]
    public void Global_object_reference_json_round_trips_with_canonical_wire_types()
    {
        var actor = new ActorRef(
            "actor-1",
            7,
            "mesh",
            RoutingId.From("node"));
        var spot = new SpotRef(
            "spot-1",
            9,
            "mesh",
            RoutingId.From("node"));

        Assert.Equal(
            "{\"actorId\":\"actor-1\",\"objectGeneration\":\"7\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}",
            JsonSerializer.Serialize(actor));
        Assert.Equal(
            "{\"spotId\":\"spot-1\",\"objectGeneration\":\"9\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}",
            JsonSerializer.Serialize(spot));
        Assert.Equal(
            actor,
            JsonSerializer.Deserialize<ActorRef>(
                "{\"actorId\":\"actor-1\",\"objectGeneration\":\"7\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}"));
        Assert.Equal(
            spot,
            JsonSerializer.Deserialize<SpotRef>(
                "{\"spotId\":\"spot-1\",\"objectGeneration\":\"9\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}"));
    }

    [Theory]
    [InlineData("{\"actorId\":\"actor-1\",\"objectGeneration\":7,\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}")]
    [InlineData("{\"actorId\":\"actor-1\",\"objectGeneration\":\"07\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}")]
    [InlineData("{\"actorId\":\"actor-1\",\"objectGeneration\":\"7\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\",\"extra\":true}")]
    [InlineData("{\"actorId\":\"actor-1\",\"objectGeneration\":\"7\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\",\"actorId\":\"other\"}")]
    public void Global_object_reference_json_rejects_noncontract_input(string json)
    {
        Assert.ThrowsAny<JsonException>(() => JsonSerializer.Deserialize<ActorRef>(json));
    }

    [Theory]
    [InlineData("{\"actorId\":\"\",\"objectGeneration\":\"1\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}")]
    [InlineData("{\"actorId\":\"\\u0000\",\"objectGeneration\":\"1\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}")]
    [InlineData("{\"actorId\":\"actor-1\",\"objectGeneration\":\"9223372036854775808\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}")]
    [InlineData("{\"actorId\":\"actor-1\",\"objectGeneration\":\"1\",\"meshName\":\"\",\"nodeRid\":\"6e6f6465\"}")]
    [InlineData("{\"actorId\":\"actor-1\",\"objectGeneration\":\"1\",\"meshName\":\"mesh\",\"nodeRid\":\"\"}")]
    public void Actor_reference_json_rejects_invalid_identity_values(string json)
    {
        Assert.ThrowsAny<JsonException>(() => JsonSerializer.Deserialize<ActorRef>(json));
    }

    [Theory]
    [InlineData("{\"spotId\":\"\",\"objectGeneration\":\"1\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}")]
    [InlineData("{\"spotId\":\"\\u0000\",\"objectGeneration\":\"1\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}")]
    [InlineData("{\"spotId\":\"spot-1\",\"objectGeneration\":\"0\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}")]
    [InlineData("{\"spotId\":\"spot-1\",\"objectGeneration\":\"9223372036854775808\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}")]
    [InlineData("{\"spotId\":\"spot-1\",\"objectGeneration\":\"1\",\"meshName\":\"\",\"nodeRid\":\"6e6f6465\"}")]
    [InlineData("{\"spotId\":\"spot-1\",\"objectGeneration\":\"1\",\"meshName\":\"mesh\",\"nodeRid\":\"\"}")]
    public void Spot_reference_json_rejects_invalid_identity_values(string json)
    {
        Assert.ThrowsAny<JsonException>(() => JsonSerializer.Deserialize<SpotRef>(json));
    }

    [Fact]
    public void Global_object_reference_json_rejects_invalid_default_values_on_encode()
    {
        Assert.ThrowsAny<ArgumentException>(() => JsonSerializer.Serialize(default(ActorRef)));
        Assert.ThrowsAny<ArgumentException>(() => JsonSerializer.Serialize(default(SpotRef)));

        var oversizedSpotId = new string('s', 256);
        var json = "{\"spotId\":\"" + oversizedSpotId
                   + "\",\"objectGeneration\":\"1\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}";
        Assert.ThrowsAny<JsonException>(() => JsonSerializer.Deserialize<SpotRef>(json));

        var actorJson = "{\"actorId\":\"" + oversizedSpotId
                        + "\",\"objectGeneration\":\"1\",\"meshName\":\"mesh\",\"nodeRid\":\"6e6f6465\"}";
        Assert.ThrowsAny<JsonException>(() => JsonSerializer.Deserialize<ActorRef>(actorJson));
    }

    [Fact]
    public void Shared_Golden_Fixture_Is_Enforced_By_The_Application_Decode_Path()
    {
        using var fixture = ReadFixture();

        foreach (var vector in fixture.RootElement.GetProperty("valid").EnumerateArray())
        {
            var payload = Encoding.UTF8.GetBytes(vector.GetProperty("jsonUtf8").GetString()!);
            using var message = Message.From(payload);
            var decoded = Assert.IsType<ProfileMessage>(ZLinkEnvelopeCodec.DecodeBody(
                message,
                typeof(ProfileMessage),
                ZLinkEnvelopeCodec.DefaultContentType,
                null));

            Assert.Equal(long.MinValue, decoded.Signed64);
            Assert.Equal(ulong.MaxValue, decoded.Unsigned64);
            Assert.Equal(ProfileState.Ready, decoded.State);
            Assert.Equal([1, 2], decoded.Bytes);
        }

        foreach (var vector in fixture.RootElement.GetProperty("invalid").EnumerateArray())
        {
            var payload = Encoding.UTF8.GetBytes(vector.GetProperty("jsonUtf8").GetString()!);
            using var message = Message.From(payload);
            Assert.ThrowsAny<JsonException>(() => ZLinkEnvelopeCodec.DecodeBody(
                message,
                typeof(ProfileMessage),
                ZLinkEnvelopeCodec.DefaultContentType,
                null));
        }
    }

    [Fact]
    public void Encoder_Uses_Canonical_64_Bit_Strings_And_Exact_Enum_Names()
    {
        var encoded = ZLinkFrameworkJsonPayloadCodec.Serialize(new ProfileMessage
        {
            Signed64 = -1,
            Unsigned64 = 2,
            Int32 = 3,
            Ratio = 1.5,
            State = ProfileState.Closed,
            Bytes = [1, 2],
            Nullable = null,
            Labels = new Dictionary<string, int> { ["a"] = 1 }
        });

        Assert.Contains("\"signed64\":\"-1\"", Encoding.UTF8.GetString(encoded));
        Assert.Contains("\"unsigned64\":\"2\"", Encoding.UTF8.GetString(encoded));
        Assert.Contains("\"state\":\"Closed\"", Encoding.UTF8.GetString(encoded));
    }

    [Theory]
    [InlineData("{\"signed64\":\"01\"}")]
    [InlineData("{\"signed64\":\"+1\"}")]
    [InlineData("{\"unsigned64\":\"01\"}")]
    [InlineData("{\"int32\":\"1\"}")]
    [InlineData("{\"labels\":{\"a\":1,\"a\":2}}")]
    public void Decoder_Rejects_Noncanonical_And_Nested_Duplicate_Input(string json)
    {
        Assert.ThrowsAny<JsonException>(() =>
            ZLinkFrameworkJsonPayloadCodec.Deserialize<ProfileMessage>(
                Encoding.UTF8.GetBytes(json)));
    }

    [Fact]
    public void Decoder_Rejects_Utf8_Bom_Before_Deserialization()
    {
        var json = Encoding.UTF8.GetBytes("{}");
        var payload = new byte[json.Length + 3];
        new byte[] { 0xef, 0xbb, 0xbf }.CopyTo(payload, 0);
        json.CopyTo(payload, 3);

        Assert.ThrowsAny<JsonException>(() =>
            ZLinkFrameworkJsonPayloadCodec.Deserialize<ProfileMessage>(payload));
    }

    [Fact]
    public void Decoder_Rejects_Null_For_A_Nonnullable_Property()
    {
        var json = """
            {"signed64":"0","unsigned64":"0","int32":0,"ratio":0,"state":"Ready","bytes":null,"nullable":null,"labels":{}}
            """;

        Assert.ThrowsAny<JsonException>(() =>
            ZLinkFrameworkJsonPayloadCodec.Deserialize<ProfileMessage>(
                Encoding.UTF8.GetBytes(json)));
    }

    [Fact]
    public void Codec_Rejects_Implicit_Language_Specific_Types()
    {
        Assert.ThrowsAny<JsonException>(() =>
            ZLinkFrameworkJsonPayloadCodec.Serialize(new UnsupportedMessage
            {
                Id = Guid.Empty
            }));
    }

    private static JsonDocument ReadFixture()
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var path = Path.GetFullPath(
            "../../runtime/protocol/golden/framework-json-v1.json",
            frameworkRoot);
        return JsonDocument.Parse(File.ReadAllText(path));
    }

    private enum ProfileState
    {
        Ready,
        Closed
    }

    private sealed class ProfileMessage
    {
        [JsonRequired]
        public long Signed64 { get; init; }

        [JsonRequired]
        public ulong Unsigned64 { get; init; }

        [JsonRequired]
        public int Int32 { get; init; }

        [JsonRequired]
        public double Ratio { get; init; }

        [JsonRequired]
        public ProfileState State { get; init; }

        [JsonRequired]
        public byte[] Bytes { get; init; } = [];

        [JsonRequired]
        public string? Nullable { get; init; }

        [JsonRequired]
        public Dictionary<string, int> Labels { get; init; } = [];
    }

    private sealed class UnsupportedMessage
    {
        public Guid Id { get; init; }
    }
}
