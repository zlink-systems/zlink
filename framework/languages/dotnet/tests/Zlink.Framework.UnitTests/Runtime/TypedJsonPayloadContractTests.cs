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

    [Theory]
    [InlineData(1024)]
    [InlineData(4096)]
    public void Bench_shaped_decode_allocation(int size)
    {
        var json = Encoding.UTF8.GetBytes(
            "{\"body\":\"" + Convert.ToBase64String(new byte[size]) + "\"}"
        );
        for (var i = 0; i < 1000; i++)
            ZLinkFrameworkJsonPayloadCodec.Deserialize<ConstructorPayload>(json);
        const int calls = 1000;
        var connector = new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Json, json);
        Check(
            "Framework generic",
            () => ZLinkFrameworkJsonPayloadCodec.Deserialize<ConstructorPayload>(json)
        );
        Check(
            "Framework declared type",
            () => ZLinkFrameworkJsonPayloadCodec.Deserialize(json, typeof(ConstructorPayload))
        );
        Check("Connector", () => connector.FromJson<ConstructorPayload>());

        void Check(string path, Func<object?> decode)
        {
            for (var i = 0; i < calls; i++)
                GC.KeepAlive(decode());
            var before = GC.GetAllocatedBytesForCurrentThread();
            for (var i = 0; i < calls; i++)
                GC.KeepAlive(decode());
            var allocated = (GC.GetAllocatedBytesForCurrentThread() - before) / calls;
            Console.WriteLine(
                $"TYPED_ALLOCATION path={path} size={size} bytes={allocated} expected={size + 48}"
            );
            Assert.Equal(size + 48, allocated);
        }
    }

    public sealed record ConstructorPayload(byte[] Body);

    public sealed class PropertyPayload
    {
        public byte[] Body { get; init; } = [];
    }
}
