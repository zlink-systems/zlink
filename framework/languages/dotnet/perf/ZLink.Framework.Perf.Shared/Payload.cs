using System.Diagnostics;
using System.Globalization;
using System.Text.Json;

namespace ZLink.Framework.Perf;

public static class DecimalText
{
    public static string Of(long value) => value.ToString(CultureInfo.InvariantCulture);
    public static string Of(ulong value) => value.ToString(CultureInfo.InvariantCulture);
    public static ulong U64(string text) => ulong.TryParse(text, NumberStyles.None,
        CultureInfo.InvariantCulture, out var value) && text == Of(value) ? value :
        throw new JsonException("Noncanonical U64 decimal string.");
    public static long I64(string text) => long.TryParse(text, NumberStyles.AllowLeadingSign,
        CultureInfo.InvariantCulture, out var value) && text == Of(value) ? value :
        throw new JsonException("Noncanonical I64 decimal string.");
}

public static class PerfClock
{
    public static readonly string Domain = $"process-{Environment.ProcessId}-{Guid.NewGuid():N}";
    public static long Now => checked((long)((Int128)Stopwatch.GetTimestamp() * 1_000_000_000 / Stopwatch.Frequency));
    public static string UnixMs => DecimalText.Of(DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
    public static ClockMetadata Metadata => new("System.Diagnostics.Stopwatch.GetTimestamp",
        DecimalText.Of((ulong)Stopwatch.Frequency), "ns", Domain, "process", null, null, null, null,
        ["Stopwatch.IsHighResolution=" + Stopwatch.IsHighResolution, "RTT uses only this process clock; remote receivedTicks is diagnostic."]);
}

public sealed class PayloadPattern(int size)
{
    public string Base64 { get; } = Generate(size);
    private static string Generate(int length)
    {
        var bytes = new byte[length];
        for (var i = 0; i < length; i++) bytes[i] = (byte)((31 * i + 17 * (i / 251) + 29) % 256);
        return Convert.ToBase64String(bytes);
    }
    public void Validate(string payload)
    {
        Span<byte> bytes = stackalloc byte[size];
        if (!Convert.TryFromBase64String(payload, bytes, out var length) || length != size ||
            !string.Equals(payload, Base64, StringComparison.Ordinal))
            throw new PerfValidationException("PayloadMismatch", "Payload is not the canonical Base64 pattern.");
        for (var i = 0; i < length; i++)
            if (bytes[i] != (byte)((31 * i + 17 * (i / 251) + 29) % 256))
                throw new PerfValidationException("PayloadMismatch", "Logical byte pattern differs.");
    }
    public static void ValidateIdentity(PerfEchoRequest request, PerfEchoReply reply)
    {
        if (request.runId != reply.runId || request.cellId != reply.cellId ||
            request.resetSeq != reply.resetSeq || request.phase != reply.phase ||
            request.clientId != reply.clientId || request.sequence != reply.sequence ||
            request.correlationId != reply.correlationId || string.IsNullOrEmpty(reply.clockDomainId))
            throw new PerfValidationException("IdentityMismatch", "Echo identity differs from the submitted operation.");
        DecimalText.I64(reply.receivedTicks);
    }
    public static PerfEchoReply Reply(PerfEchoRequest request, long receivedTicks) => new()
    {
        runId = request.runId, cellId = request.cellId, resetSeq = request.resetSeq, phase = request.phase,
        clientId = request.clientId, sequence = request.sequence, correlationId = request.correlationId,
        receivedTicks = DecimalText.Of(receivedTicks), clockDomainId = PerfClock.Domain, payload = request.payload
    };
}
public sealed class PerfValidationException(string kind, string message) : Exception(message)
{
    public string Kind { get; } = kind;
}
