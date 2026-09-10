using System.Buffers.Binary;
using Google.Protobuf;
using WithGrpcBench.Shared;

const string golden = "0a1d4b4e4c5a04030201011d00000008070605040302011817161514131211";
foreach (var size in new[] { 29, 127, 128, 1024 })
{
    var bytes = BenchMetricHeaders.CreatePayloadBytes(size, 0x01020304, BenchPhase.Active, 0x0102030405060708);
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
        if (hex != golden) throw new InvalidOperationException("Fixed pre-change dump mismatch.");
        Console.WriteLine($"dotnet raw wire 29B: {hex}");
    }
}
Console.WriteLine("dotnet raw wire identity: 29, 127, 128, 1024 bytes PASS");
