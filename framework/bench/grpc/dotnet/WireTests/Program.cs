using System.Buffers.Binary;
using System.Reflection;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Google.Protobuf;
using WithGrpcBench.Shared;
using Zlink.Framework.Contracts.Codecs;

const string golden = "0a1d4b4e4c5a04030201011d00000008070605040302011817161514131211";
foreach (var size in new[] { 29, 127, 128, 1024 })
{
    var bytes = BenchMetricHeaders.CreatePayloadBytes(
        size,
        0x01020304,
        BenchPhase.Active,
        0x0102030405060708
    );
    BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(21), 0x1112131415161718);
    // The historical raw framing is retained only as the wire compatibility oracle.
    var legacy = new List<byte> { 0x0a };
    var length = size;
    while (length >= 0x80)
    {
        legacy.Add((byte)((length & 0x7f) | 0x80));
        length >>= 7;
    }
    legacy.Add((byte)length);
    legacy.AddRange(bytes);
    var payload = new BenchPayload { Body = ByteString.CopyFrom(bytes) };
    // Matches the existing .NET raw client's CalculateSize/WriteTo path.
    var encoded = new byte[payload.CalculateSize()];
    RawWire.Encode(payload, encoded);
    if (!encoded.AsSpan().SequenceEqual(legacy.ToArray()))
        throw new InvalidOperationException("Legacy/protobuf wire mismatch.");
    var decoded = RawWire.Decode(encoded);
    var reply = new BenchPayload { Body = decoded.Body };
    var replyBytes = new byte[reply.CalculateSize()];
    RawWire.Encode(reply, replyBytes);
    if (!decoded.Body.Equals(payload.Body) || !replyBytes.AsSpan().SequenceEqual(encoded))
        throw new InvalidOperationException("Protobuf parse/reply wire mismatch.");
    if (size == 29)
    {
        var hex = Convert.ToHexString(encoded).ToLowerInvariant();
        if (hex != golden)
            throw new InvalidOperationException("Fixed pre-change dump mismatch.");
        Console.WriteLine($"dotnet raw wire 29B: {hex}");
    }
}
Console.WriteLine("dotnet raw wire identity: 29, 127, 128, 1024 bytes PASS");

// The default codec is internal. Reflection is confined to this test oracle.
var codecType = Assembly
    .Load("Zlink.Framework")
    .GetType(
        "Zlink.Framework.Runtime.Messaging.ZLinkFrameworkJsonPayloadCodec",
        throwOnError: true
    )!;
var frameworkJson = (IZLinkMessageSerializer)
    codecType
        .GetProperty("ReceiveSerializer", BindingFlags.Static | BindingFlags.NonPublic)!
        .GetValue(null)!;

var serializeFrameworkPart = codecType
    .GetMethod("SerializePart", BindingFlags.Static | BindingFlags.NonPublic)!
    .CreateDelegate<Func<object?, Type, Systems.Zlink.Message>>();

using (
    var fixture = JsonDocument.Parse(
        File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "ladder-mixed-payload.json"))
    )
)
{
    var fixtureGolden = fixture.RootElement.GetProperty("golden");
    foreach (var payloadSize in new[] { 1024, 4096 })
    {
        var expected = fixtureGolden
            .GetProperty(payloadSize.ToString())
            .GetProperty("json")
            .GetString()!;
        var itemCount = fixtureGolden
            .GetProperty(payloadSize.ToString())
            .GetProperty("itemCount")
            .GetInt32();
        var byteLength = fixtureGolden
            .GetProperty(payloadSize.ToString())
            .GetProperty("byteLength")
            .GetInt32();
        if (BenchGamePayloads.ItemCount(payloadSize) != itemCount)
            throw new InvalidOperationException(
                $"Game item count differs from the {payloadSize} fixture."
            );
        var game = BenchGamePayloads.Create(
            123,
            BenchPhase.Active,
            456,
            1_700_000_000_000_000_000,
            itemCount
        );
        var expectedBytes = Encoding.UTF8.GetBytes(expected);
        if (expectedBytes.Length != byteLength)
            throw new InvalidOperationException($"Fixture byte length differs for {payloadSize}.");
        var rawBytes = BenchGamePayloads.EncodeStandard(game);
        var frameworkBytes = frameworkJson
            .Serialize(game, typeof(BenchGameMessage))
            .Bytes.ToArray();
        if (!rawBytes.AsSpan().SequenceEqual(expectedBytes))
            throw new InvalidOperationException(
                $"Raw System.Text.Json game bytes differ from the {payloadSize} fixture golden."
            );
        if (!frameworkBytes.AsSpan().SequenceEqual(expectedBytes))
            throw new InvalidOperationException(
                $"Framework typed JSON game bytes differ from the {payloadSize} fixture golden."
            );
        var decodedRaw = BenchGamePayloads.DecodeStandard(expectedBytes);
        var decodedFramework = (BenchGameMessage)
            frameworkJson.Deserialize(
                ZLinkEncodedPayload.From(expectedBytes),
                typeof(BenchGameMessage)
            )!;
        if (
            !BenchGamePayloads.HasExpectedStamp(decodedRaw, 123, BenchPhase.Active, 456)
            || !BenchGamePayloads.HasExpectedStamp(decodedFramework, 123, BenchPhase.Active, 456)
        )
            throw new InvalidOperationException(
                $"Game JSON stamp decode failed for {payloadSize}."
            );
        if (
            !BenchGamePayloads.EncodeStandard(decodedRaw).AsSpan().SequenceEqual(expectedBytes)
            || !frameworkJson
                .Serialize(decodedFramework, typeof(BenchGameMessage))
                .Bytes.Span.SequenceEqual(expectedBytes)
        )
            throw new InvalidOperationException("JSON decoder changed body fields.");
        var valid = JsonNode.Parse(expected)!.AsObject();
        var invalid = new List<JsonObject>();
        foreach (var name in valid.Select(property => property.Key))
        {
            var missing = (JsonObject)valid.DeepClone();
            missing.Remove(name);
            invalid.Add(missing);
            var nullField = (JsonObject)valid.DeepClone();
            nullField[name] = null;
            invalid.Add(nullField);
        }
        foreach (var name in valid["items"]![0]!.AsObject().Select(property => property.Key))
        {
            var missing = (JsonObject)valid.DeepClone();
            missing["items"]![0]!.AsObject().Remove(name);
            invalid.Add(missing);
            var nullField = (JsonObject)valid.DeepClone();
            nullField["items"]![0]![name] = null;
            invalid.Add(nullField);
        }
        foreach (var gold in new JsonNode[] { JsonValue.Create("01")!, JsonValue.Create(1)! })
        {
            var bad = (JsonObject)valid.DeepClone();
            bad["gold"] = gold;
            invalid.Add(bad);
        }
        var stringHeading = (JsonObject)valid.DeepClone();
        stringHeading["heading"] = "1";
        invalid.Add(stringHeading);
        foreach (var bad in invalid)
        {
            var bytes = Encoding.UTF8.GetBytes(bad.ToJsonString());
            var rawRejected = false;
            var frameworkRejected = false;
            try
            {
                BenchGamePayloads.DecodeStandard(bytes);
            }
            catch (JsonException)
            {
                rawRejected = true;
            }
            try
            {
                frameworkJson.Deserialize(
                    ZLinkEncodedPayload.From(bytes),
                    typeof(BenchGameMessage)
                );
            }
            catch (JsonException)
            {
                frameworkRejected = true;
            }
            if (!rawRejected || !frameworkRejected)
                throw new InvalidOperationException($"JSON rejection parity failed: {bad}");
        }
        Console.WriteLine(
            $"dotnet game JSON fixture {payloadSize}: binding + Framework typed JSON bytes, decode, rejection parity PASS ({expectedBytes.Length} bytes)"
        );
    }
}
