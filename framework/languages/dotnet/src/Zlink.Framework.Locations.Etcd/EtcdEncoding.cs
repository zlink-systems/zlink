using System.Buffers.Binary;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.RegularExpressions;
using Google.Protobuf;

[assembly: InternalsVisibleTo("Zlink.Framework.Locations.Etcd.Tests")]

namespace Zlink.Framework.Locations.Etcd;

internal readonly record struct EtcdMetadata(bool HasExpiry, long ExpiresAt, int PayloadLength)
{
    internal bool IsExpired(long now) => HasExpiry && ExpiresAt <= now;
}

internal static class EtcdEncoding
{
    internal const long MaxTime = 9_223_372_036_854;
    internal const int MaxPayload = 1_048_576;
    internal const int MaxPageBytes = 4 * 1024 * 1024;
    internal static readonly UTF8Encoding Utf8 = new(false, true);

    internal static IOException Failure() =>
        new("The Location Store operation failed; its write outcome may be unknown.");

    internal static ByteString Key(string ns, byte kind, ByteString key)
    {
        var prefix = Utf8.GetBytes(ns + "\0" + (char)kind + (kind == (byte)'c' ? "" : "\0"));
        return ByteString.CopyFrom(prefix.Concat(key).ToArray());
    }

    internal static ByteString PrefixEnd(ByteString prefix)
    {
        var bytes = prefix.ToByteArray();
        for (var i = bytes.Length - 1; i >= 0; i--)
            if (bytes[i] != 255)
            {
                bytes[i]++;
                return ByteString.CopyFrom(bytes, 0, i + 1);
            }
        return ByteString.CopyFrom(new byte[] { 0 });
    }

    internal static ByteString Clock(long value)
    {
        if (value is < 0 or > MaxTime)
            throw new ArgumentException("Store time is outside its encoding range.");
        var bytes = new byte[8];
        BinaryPrimitives.WriteUInt64BigEndian(bytes, (ulong)value);
        return ByteString.CopyFrom(bytes);
    }

    internal static long DecodeClock(ByteString bytes)
    {
        if (bytes.Length != 8)
            throw Failure();
        var value = BinaryPrimitives.ReadUInt64BigEndian(bytes.Span);
        return value <= MaxTime ? (long)value : throw Failure();
    }

    internal static ByteString Metadata(bool hasExpiry, long expiry, int length)
    {
        if (length is < 0 or > MaxPayload || (!hasExpiry && expiry != 0))
            throw new ArgumentException("Invalid metadata.");
        var bytes = new byte[14];
        bytes[0] = 1;
        bytes[1] = hasExpiry ? (byte)1 : (byte)0;
        Clock(expiry).Span.CopyTo(bytes.AsSpan(2));
        BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(10), (uint)length);
        return ByteString.CopyFrom(bytes);
    }

    internal static EtcdMetadata DecodeMetadata(ByteString bytes)
    {
        if (bytes.Length != 14 || bytes[0] != 1 || bytes[1] > 1)
            throw Failure();
        var expiry = DecodeClock(ByteString.CopyFrom(bytes.Span.Slice(2, 8)));
        var length = BinaryPrimitives.ReadUInt32BigEndian(bytes.Span[10..]);
        if (length > MaxPayload || (bytes[1] == 0 && expiry != 0))
            throw Failure();
        return new(bytes[1] == 1, expiry, (int)length);
    }

    internal static string Version(ulong cluster, long revision)
    {
        if (revision <= 0)
            throw Failure();
        return $"e1:{cluster:x16}:{revision.ToString(CultureInfo.InvariantCulture)}";
    }

    internal static (ulong Cluster, long Revision)? DecodeVersion(string value)
    {
        if (
            !Regex.IsMatch(
                value,
                "\\Ae1:[0-9a-f]{16}:[1-9][0-9]*\\z",
                RegexOptions.CultureInvariant
            )
            || !long.TryParse(
                value.AsSpan(20),
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out var revision
            )
        )
            return null;
        return (
            ulong.Parse(value.AsSpan(3, 16), NumberStyles.HexNumber, CultureInfo.InvariantCulture),
            revision
        );
    }

    internal static ByteString ValidateKey(string? key, bool prefix = false)
    {
        if (key is null)
            throw new ArgumentException("A Store key is required.");
        byte[] bytes;
        try
        {
            bytes = Utf8.GetBytes(key);
        }
        catch (EncoderFallbackException)
        {
            throw new ArgumentException("A Store key must be valid UTF-8.");
        }
        if (bytes.Length > 1024 || (!prefix && bytes.Length == 0))
            throw new ArgumentException("A Store key exceeds its UTF-8 bound.");
        return ByteString.CopyFrom(bytes);
    }
}
