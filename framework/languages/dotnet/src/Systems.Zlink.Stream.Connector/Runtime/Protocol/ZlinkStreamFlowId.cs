using System.Security.Cryptography;

namespace Systems.Zlink.Stream.Connector.Runtime.Protocol;

internal static class ZlinkStreamFlowId
{
    private const int UuidSize = 16;
    private const int BitsPerByte = 8;
    private const int VersionByteIndex = 6;
    private const int VariantByteIndex = 8;
    private const byte VersionClearMask = 0x0f;
    private const byte Version7Bits = 0x70;
    private const byte VariantClearMask = 0x3f;
    private const byte RfcVariantBits = 0x80;
    private const int VersionTextIndex = 14;
    private const int VariantTextIndex = 19;
    private const char Version7Text = '7';
    private const char VariantText8 = '8';
    private const char VariantText9 = '9';
    private const char VariantTextA = 'a';
    private const char VariantTextB = 'b';
    private const string UuidFormat = "D";
    public const byte FormatMarker = 0xF2;
    public const int EncodedLength = 36;

    public static string Create()
    {
        Span<byte> bytes = stackalloc byte[UuidSize];
        RandomNumberGenerator.Fill(bytes);

        var milliseconds = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        bytes[0] = (byte)(milliseconds >> (5 * BitsPerByte));
        bytes[1] = (byte)(milliseconds >> (4 * BitsPerByte));
        bytes[2] = (byte)(milliseconds >> (3 * BitsPerByte));
        bytes[3] = (byte)(milliseconds >> (2 * BitsPerByte));
        bytes[4] = (byte)(milliseconds >> (1 * BitsPerByte));
        bytes[5] = (byte)milliseconds;
        bytes[VersionByteIndex] = (byte)(
            (bytes[VersionByteIndex] & VersionClearMask) | Version7Bits
        );
        bytes[VariantByteIndex] = (byte)(
            (bytes[VariantByteIndex] & VariantClearMask) | RfcVariantBits
        );

        return new Guid(
            (bytes[0] << (3 * BitsPerByte))
                | (bytes[1] << (2 * BitsPerByte))
                | (bytes[2] << (1 * BitsPerByte))
                | bytes[3],
            (short)((bytes[4] << (1 * BitsPerByte)) | bytes[5]),
            (short)((bytes[6] << (1 * BitsPerByte)) | bytes[7]),
            bytes[8],
            bytes[9],
            bytes[10],
            bytes[11],
            bytes[12],
            bytes[13],
            bytes[14],
            bytes[15]
        ).ToString(UuidFormat);
    }

    public static bool IsValid(string? value)
    {
        return value is { Length: EncodedLength }
            && value == value.ToLowerInvariant()
            && value[VersionTextIndex] == Version7Text
            && value[VariantTextIndex]
                is VariantText8
                    or VariantText9
                    or VariantTextA
                    or VariantTextB
            && Guid.TryParseExact(value, UuidFormat, out _);
    }
}
