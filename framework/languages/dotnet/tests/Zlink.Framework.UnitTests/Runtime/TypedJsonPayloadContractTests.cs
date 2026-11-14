using System.Text;
using System.Text.Json;
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

    public sealed record ConstructorPayload(byte[] Body);

    public sealed class PropertyPayload
    {
        public byte[] Body { get; init; } = [];
    }
}
