using System.Buffers.Binary;
using System.Diagnostics;
using Google.Protobuf;
using WithGrpcBench.Shared;

// Compare the absolute monotonic time, not two equally overflowed readings.
var lowerTimestampNs = (long)((Int128)Stopwatch.GetTimestamp() * 1_000_000_000L / Stopwatch.Frequency);
var timestampNs = BenchMetricHeaders.NowNs();
var upperTimestampNs = (long)((Int128)Stopwatch.GetTimestamp() * 1_000_000_000L / Stopwatch.Frequency);
if (timestampNs < lowerTimestampNs || timestampNs > upperTimestampNs)
    throw new InvalidOperationException("Metric nanoseconds overflow or differ from the monotonic clock.");
Console.WriteLine("dotnet metric monotonic nanoseconds: PASS");

const string golden = "0a1d4b4e4c5a04030201011d00000008070605040302011817161514131211";
foreach (var size in new[] { 29, 64, 127, 128, 1024, 4096 })
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
Console.WriteLine("dotnet raw wire identity: 29, 64, 127, 128, 1024, 4096 bytes PASS");

var request = BenchMetricHeaders.CreateRequestPayload(7, BenchPhase.Active, 11);
var response = BenchMetricHeaders.CreateResponsePayload(request);
if (request.Body.Length != 64 || response.Body.Length != 4096
    || !BenchMetricHeaders.TryDecode(request, out var requestHeader)
    || !BenchMetricHeaders.TryDecode(response, out var responseHeader)
    || !BenchMetricHeaders.IsExpected(responseHeader, 7, BenchPhase.Active, 4096, 11)
    || responseHeader.SentTimestampNs != requestHeader.SentTimestampNs)
    throw new InvalidOperationException("64-byte request / 4096-byte response header mismatch.");
Console.WriteLine("dotnet request64/response4096 metric identity: PASS");
