using System.Buffers.Binary;
using Google.Protobuf;
using WithGrpcBench.Shared;

const string golden = "0a1d4b4e4c5a04030201011d00000008070605040302011817161514131211";
foreach (var size in new[] { 29, 127, 128, 1024, 4096 })
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
    var payload = new BenchPayload { Body = UnsafeByteOperations.UnsafeWrap(bytes) };
    var typedSource = new BenchTypedPayload(bytes);
    if (
        !System.Runtime.InteropServices.MemoryMarshal.TryGetArray(
            payload.Body.Memory,
            out var storage
        )
        || !ReferenceEquals(bytes, storage.Array)
        || !ReferenceEquals(bytes, typedSource.Body)
    )
        throw new InvalidOperationException(
            "Protobuf and typed JSON must share the source byte array."
        );
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
Console.WriteLine("dotnet raw wire identity: 29, 127, 128, 1024, 4096 bytes PASS");

var typedBody = BenchMetricHeaders.CreatePayloadBytes(127, 7, BenchPhase.Active, 42);
var typedPayload = new BenchTypedPayload(typedBody);
var jsonOptions = new System.Text.Json.JsonSerializerOptions(
    System.Text.Json.JsonSerializerDefaults.Web
);
var typedJson = System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(typedPayload, jsonOptions);
var typedJsonText = System.Text.Encoding.UTF8.GetString(typedJson);
if (!typedJsonText.StartsWith("{\"body\":\"", StringComparison.Ordinal))
    throw new InvalidOperationException("Typed DTO did not use the default JSON body field.");
var decodedTypedPayload = System.Text.Json.JsonSerializer.Deserialize<BenchTypedPayload>(
    typedJson,
    jsonOptions
);
if (decodedTypedPayload is null || !decodedTypedPayload.Body.AsSpan().SequenceEqual(typedBody))
    throw new InvalidOperationException("Typed JSON did not preserve arbitrary payload bytes.");
Console.WriteLine("dotnet typed DTO JSON/base64 roundtrip: PASS");
