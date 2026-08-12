using System.Text;

namespace Zlink.Framework.Runtime.Locations;

internal enum ZLinkAuthorityKeyKind
{
    Actor,
    Spot
}

internal static class ZLinkAuthorityKeyCodec
{
    private const int MaximumIdentityBytes = byte.MaxValue;
    private const int MaximumEncodedKeyBytes = 776;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    internal static ZLinkAuthorityKey EncodeActor(string actorId) =>
        Encode(ZLinkAuthorityKeyKind.Actor, actorId, nameof(actorId));

    internal static ZLinkAuthorityKey EncodeSpot(string spotId) =>
        Encode(ZLinkAuthorityKeyKind.Spot, spotId, nameof(spotId));

    internal static bool TryDecodeActor(
        ZLinkAuthorityKey key,
        out string actorId) =>
        TryDecode(key, ZLinkAuthorityKeyKind.Actor, out actorId);

    internal static bool TryDecodeSpot(
        ZLinkAuthorityKey key,
        out string spotId) =>
        TryDecode(key, ZLinkAuthorityKeyKind.Spot, out spotId);

    internal static string DecodeActor(ZLinkAuthorityKey key) =>
        Decode(key, ZLinkAuthorityKeyKind.Actor);

    internal static string DecodeSpot(ZLinkAuthorityKey key) =>
        Decode(key, ZLinkAuthorityKeyKind.Spot);

    private static ZLinkAuthorityKey Encode(
        ZLinkAuthorityKeyKind kind,
        string identity,
        string parameterName)
    {
        ArgumentNullException.ThrowIfNull(identity, parameterName);
        byte[] bytes;
        try
        {
            bytes = StrictUtf8.GetBytes(identity);
        }
        catch (EncoderFallbackException error)
        {
            throw new ArgumentException(
                "Authority identity must be valid UTF-8 text.",
                parameterName,
                error);
        }

        if (bytes.Length is 0 or > MaximumIdentityBytes
            || identity.Contains('\0'))
            throw new ArgumentOutOfRangeException(parameterName);

        var discriminator = kind == ZLinkAuthorityKeyKind.Actor ? 'a' : 's';
        var builder = new StringBuilder($"zla1:{discriminator}:{bytes.Length}:");
        foreach (var value in bytes)
        {
            if (IsUnreserved(value))
                builder.Append((char)value);
            else
                builder.Append('%').Append(value.ToString("X2"));
        }

        return new ZLinkAuthorityKey(builder.ToString());
    }

    private static bool TryDecode(
        ZLinkAuthorityKey key,
        ZLinkAuthorityKeyKind expectedKind,
        out string identity)
    {
        identity = string.Empty;
        var encodedKey = key.Value;
        if (string.IsNullOrEmpty(encodedKey)
            || encodedKey.Length > MaximumEncodedKeyBytes)
            return false;

        var discriminator = expectedKind == ZLinkAuthorityKeyKind.Actor ? 'a' : 's';
        var prefix = $"zla1:{discriminator}:";
        if (!encodedKey.StartsWith(prefix, StringComparison.Ordinal))
            return false;

        var lengthEnd = encodedKey.IndexOf(':', prefix.Length);
        if (lengthEnd < 0
            || !TryParseCanonicalLength(
                encodedKey.AsSpan(prefix.Length, lengthEnd - prefix.Length),
                out var expectedLength))
            return false;

        var encodedIdentity = encodedKey.AsSpan(lengthEnd + 1);
        if (encodedIdentity.Length < expectedLength
            || encodedIdentity.Length > expectedLength * 3)
            return false;

        Span<byte> bytes = stackalloc byte[MaximumIdentityBytes];
        var byteCount = 0;
        for (var index = 0; index < encodedIdentity.Length;)
        {
            if (byteCount == expectedLength)
                return false;

            var value = encodedIdentity[index];
            if (value != '%')
            {
                if (value > 0x7f || !IsUnreserved((byte)value))
                    return false;
                bytes[byteCount++] = (byte)value;
                index++;
                continue;
            }

            if (index + 2 >= encodedIdentity.Length
                || !TryUpperHex(encodedIdentity[index + 1], out var high)
                || !TryUpperHex(encodedIdentity[index + 2], out var low))
                return false;
            var decoded = checked((byte)((high << 4) | low));
            if (IsUnreserved(decoded))
                return false;
            bytes[byteCount++] = decoded;
            index += 3;
        }

        if (byteCount != expectedLength)
            return false;

        try
        {
            identity = StrictUtf8.GetString(bytes[..byteCount]);
            if (identity.Length == 0 || identity.Contains('\0'))
            {
                identity = string.Empty;
                return false;
            }
            return true;
        }
        catch (DecoderFallbackException)
        {
            identity = string.Empty;
            return false;
        }
    }

    private static string Decode(
        ZLinkAuthorityKey key,
        ZLinkAuthorityKeyKind expectedKind)
    {
        if (TryDecode(key, expectedKind, out var identity))
            return identity;
        throw new InvalidDataException(
            $"Authority key '{key.Value}' is not canonical authority-key-v1.");
    }

    private static bool TryParseCanonicalLength(
        ReadOnlySpan<char> text,
        out int length)
    {
        length = 0;
        if (text.Length is 0 or > 3 || text[0] is < '1' or > '9')
            return false;
        foreach (var value in text)
        {
            if (value is < '0' or > '9')
                return false;
            length = checked(length * 10 + value - '0');
        }
        return length <= MaximumIdentityBytes;
    }

    private static bool TryUpperHex(char value, out int decoded)
    {
        if (value is >= '0' and <= '9')
        {
            decoded = value - '0';
            return true;
        }
        if (value is >= 'A' and <= 'F')
        {
            decoded = value - 'A' + 10;
            return true;
        }
        decoded = 0;
        return false;
    }

    private static bool IsUnreserved(byte value) =>
        value is >= (byte)'A' and <= (byte)'Z'
        or >= (byte)'a' and <= (byte)'z'
        or >= (byte)'0' and <= (byte)'9'
        or (byte)'-' or (byte)'.' or (byte)'_' or (byte)'~';
}
