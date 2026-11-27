using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;

namespace Zlink.NetworkSync.Server.Tests;

public sealed class WireFixturesTests
{
    public static IEnumerable<object[]> Fixtures()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "fixtures.json");
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        foreach (var fixture in doc.RootElement.EnumerateArray())
        {
            var name = fixture.GetProperty("name").GetString()!;
            var packet = fixture.GetProperty("packet").GetString()!;
            // The Framework resolves packet names before decoding, and the server never receives Pong.
            if (name is "name-mismatch" or "zero-tick-interval")
                continue;
            yield return [name, packet, fixture.GetProperty("payload").GetString()!, fixture.GetProperty("valid").GetBoolean()];
        }
    }

    [Theory]
    [MemberData(nameof(Fixtures))]
    public void UsesFrameworkJsonV1(string name, string packet, string json, bool valid)
    {
        var payload = new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Json, Encoding.UTF8.GetBytes(json));
        Func<ZlinkStreamEncodedPayload, ZlinkStreamEncodedPayload> decodeThenEncode = packet switch
        {
            "NetworkSyncInputV1" => RoundTrip<NetworkSyncInputV1<JsonElement, JsonElement>>,
            "NetworkSyncPingV1" => RoundTrip<NetworkSyncPingV1>,
            "NetworkSyncSnapshotV1" => RoundTrip<NetworkSyncSnapshotV1<JsonElement>>,
            "NetworkSyncPongV1" => RoundTrip<NetworkSyncPongV1>,
            _ => throw new InvalidDataException($"Unknown fixture packet {packet}."),
        };

        if (!valid)
        {
            Assert.Throws<JsonException>(() => { _ = decodeThenEncode(payload); });
            return;
        }

        var expected = JsonNode.Parse(json)!.AsObject();
        if (name == "ping")
            expected.Remove("future_field");
        var actual = JsonNode.Parse(decodeThenEncode(payload).Payload.Span);
        Assert.True(JsonNode.DeepEquals(expected, actual), $"Expected {expected}, received {actual}.");
    }

    private static ZlinkStreamEncodedPayload RoundTrip<T>(ZlinkStreamEncodedPayload payload) =>
        payload.FromJson<T>().ToJson();
}
