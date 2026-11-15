using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using Google.Protobuf;
using Systems.Zlink.Stream.Connector.Contracts;
using WithGrpcBench.Shared;

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
        var frameworkPayload = game.ToJson();
        var frameworkBytes = frameworkPayload.Payload.ToArray();
        if (!rawBytes.AsSpan().SequenceEqual(expectedBytes))
            throw new InvalidOperationException(
                $"Raw System.Text.Json game bytes differ from the {payloadSize} fixture golden."
            );
        if (!frameworkBytes.AsSpan().SequenceEqual(expectedBytes))
            throw new InvalidOperationException(
                $"Framework typed JSON game bytes differ from the {payloadSize} fixture golden."
            );
        var decodedRaw = BenchGamePayloads.DecodeStandard(expectedBytes);
        var decodedFramework = new ZlinkStreamEncodedPayload(
            ZlinkStreamCodec.Json,
            expectedBytes
        ).FromJson<BenchGameMessage>();
        if (
            !BenchGamePayloads.HasExpectedStamp(decodedRaw, 123, BenchPhase.Active, 456)
            || !BenchGamePayloads.HasExpectedStamp(decodedFramework, 123, BenchPhase.Active, 456)
        )
            throw new InvalidOperationException(
                $"Game JSON stamp decode failed for {payloadSize}."
            );
        Console.WriteLine(
            $"dotnet game JSON fixture {payloadSize}: raw + public Framework JSON codec encoding and decode PASS ({expectedBytes.Length} bytes)"
        );
    }
}
