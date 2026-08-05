using System.Security.Cryptography;

namespace Systems.Zlink.Stream.Connector.Runtime.Protocol;

internal static class ZlinkStreamFlowId
{
    public const byte FormatMarker = 0xF2;
    public const int EncodedLength = 36;

    public static string Create()
    {
        Span<byte> bytes = stackalloc byte[16];
        RandomNumberGenerator.Fill(bytes);

        var milliseconds = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        bytes[0] = (byte)(milliseconds >> 40);
        bytes[1] = (byte)(milliseconds >> 32);
        bytes[2] = (byte)(milliseconds >> 24);
        bytes[3] = (byte)(milliseconds >> 16);
        bytes[4] = (byte)(milliseconds >> 8);
        bytes[5] = (byte)milliseconds;
        bytes[6] = (byte)((bytes[6] & 0x0F) | 0x70);
        bytes[8] = (byte)((bytes[8] & 0x3F) | 0x80);

        return new Guid(bytes, bigEndian: true).ToString("D");
    }

    public static bool IsValid(string? value)
    {
        return value is { Length: EncodedLength }
               && value == value.ToLowerInvariant()
               && value[14] == '7'
               && value[19] is '8' or '9' or 'a' or 'b'
               && Guid.TryParseExact(value, "D", out _);
    }
}
