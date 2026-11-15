using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.UnitTests;

public sealed class TypedJsonPayloadContractTests
{
    public static IEnumerable<object[]> Rows()
    {
        var root = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        foreach (
            var line in File.ReadLines(
                Path.GetFullPath("../../test/fixtures/typed-json-payload.tsv", root)
            )
        )
        {
            if (line.StartsWith("#", StringComparison.Ordinal))
                continue;
            var fields = line.Split('\t');
            yield return new object[] { fields[0], fields[1] == "accept", fields[2], fields[3] };
        }
    }

    [Theory]
    [MemberData(nameof(Rows))]
    public void Shared_typed_contract(string id, bool accept, string shape, string json)
    {
        if (shape.StartsWith("encode-", StringComparison.Ordinal))
        {
            object value = shape switch
            {
                "encode-string" => new StringPayload(null!),
                "encode-object" => new ObjectPayload(null!),
                "encode-nullable" => new NullableReferences(null, null),
                _ => throw new InvalidOperationException(shape),
            };
            byte[]? wire = null;
            var error = Record.Exception(() =>
                wire = ZLinkFrameworkJsonPayloadCodec.Serialize(value, value.GetType())
            );
            Console.WriteLine(
                $"ENCODE_CONTRACT {id} wire={Encoding.UTF8.GetString(wire ?? [])} error={error}"
            );
            Assert.Equal(accept, error is null);
            if (accept)
                Assert.Equal(json, Encoding.UTF8.GetString(wire!));
            else
                Assert.Equal(
                    "Property 'wireValue' does not allow null.",
                    Assert.IsAssignableFrom<JsonException>(error).Message
                );
            byte[]? connectorWire = null;
            var encodeConnectorError = Record.Exception(() =>
                connectorWire = value.ToJson().Payload.ToArray()
            );
            Assert.Equal(accept, encodeConnectorError is null);
            if (accept)
                Assert.Equal(json, Encoding.UTF8.GetString(connectorWire!));
            else
                Assert.Equal(
                    "Property 'wireValue' does not allow null.",
                    Assert.IsAssignableFrom<JsonException>(encodeConnectorError).Message
                );
            return;
        }
        var bytes = Encoding.UTF8.GetBytes(json);
        var type = shape == "ctor" ? typeof(ConstructorPayload) : typeof(PropertyPayload);
        var frameworkError = Record.Exception(() =>
            ZLinkFrameworkJsonPayloadCodec.Deserialize(bytes, type)
        );
        var connectorError = Record.Exception(() =>
        {
            var payload = new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Json, bytes);
            if (shape == "ctor")
                payload.FromJson<ConstructorPayload>();
            else
                payload.FromJson<PropertyPayload>();
        });
        Assert.True(
            accept == (frameworkError is null),
            $"{id}: Framework expected accept={accept}, error={frameworkError}"
        );
        Assert.True(
            accept == (connectorError is null),
            $"{id}: Connector expected accept={accept}, error={connectorError}"
        );
        if (!accept)
        {
            Assert.IsAssignableFrom<JsonException>(frameworkError);
            Assert.IsAssignableFrom<JsonException>(connectorError);
        }
    }

    public sealed record StringPayload([property: JsonPropertyName("wireValue")] string Value);

    public sealed record Child(string Text);

    public sealed record ObjectPayload([property: JsonPropertyName("wireValue")] Child Value);

    public sealed record NullableReferences(string? Text, Child? Child);

    public sealed record ConstructorPayload(byte[] Body);

    public sealed class PropertyPayload
    {
        public byte[] Body { get; init; } = [];
    }
}
