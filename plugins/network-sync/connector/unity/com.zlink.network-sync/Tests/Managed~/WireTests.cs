// SPDX-License-Identifier: LicenseRef-Zlink
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;

namespace Zlink.NetworkSync.Tests;

public sealed class WireTests
{
    public static IEnumerable<object[]> Fixtures()
    {
        using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "fixtures.json")));
        foreach (var row in document.RootElement.EnumerateArray())
        {
            yield return new object[]
            {
                row.GetProperty("name").GetString()!,
                row.GetProperty("packet").GetString()!,
                row.GetProperty("payload").GetString()!,
                row.GetProperty("valid").GetBoolean(),
            };
        }
    }

    [Theory]
    [MemberData(nameof(Fixtures))]
    public void UsesConnectorJsonProfile(string name, string packet, string json, bool valid)
    {
        if (!valid)
        {
            Assert.ThrowsAny<JsonException>(() => RoundTrip(packet, json));
            return;
        }

        var expected = JsonNode.Parse(json)!;
        if (name == "ping")
            expected.AsObject().Remove("future_field");

        var actual = JsonNode.Parse(RoundTrip(packet, json).Payload.Span)!;
        Assert.True(JsonNode.DeepEquals(expected, actual), $"Fixture {name} produced {actual}");
    }

    private static ZlinkStreamEncodedPayload RoundTrip(string packet, string json)
    {
        var payload = new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Json, Encoding.UTF8.GetBytes(json));
        return packet switch
        {
            NetworkSyncPackets.Input => payload.FromJson<NetworkSyncInputV1<JsonElement, JsonElement>>().ToJson(),
            NetworkSyncPackets.Ping => payload.FromJson<NetworkSyncPingV1>().ToJson(),
            NetworkSyncPackets.Snapshot => payload.FromJson<NetworkSyncSnapshotV1<JsonElement>>().ToJson(),
            NetworkSyncPackets.Pong => payload.FromJson<NetworkSyncPongV1>().ToJson(),
            _ => throw new JsonException($"Unknown network sync packet {packet}."),
        };
    }
}
