// SPDX-License-Identifier: MPL-2.0

using System.Buffers.Binary;
using System.Globalization;
using System.Text;

namespace Systems.Zlink;

/// <summary>
///     An opaque identifier for a messaging peer or route, 1 to 255 bytes long.
/// </summary>
public readonly partial struct RoutingId : IEquatable<RoutingId>
{
    private const int MaxSize = 255;
    private const int MaxHexStringLength = MaxSize * 2;
    private readonly byte[]? _bytes;
    private readonly int _hash;

    private RoutingId(byte[] bytes, bool takeOwnership)
    {
        _bytes = takeOwnership ? bytes : bytes.ToArray();
        _hash = ComputeHash(_bytes);
    }

    /// <summary>
    ///     Creates a routing id from a copy of the given bytes (1 to 255 bytes).
    /// </summary>
    public static RoutingId From(ReadOnlySpan<byte> bytes)
    {
        Validate(bytes, nameof(bytes));
        return new RoutingId(bytes.ToArray(), true);
    }

    /// <summary>
    ///     Creates a routing id from a copy of the given bytes (1 to 255 bytes).
    /// </summary>
    public static RoutingId From(byte[] bytes)
    {
        if (bytes == null)
            throw new ArgumentNullException(nameof(bytes));
        Validate(bytes, nameof(bytes));
        return new RoutingId(bytes, false);
    }

    /// <summary>
    ///     Creates a routing id from the UTF-8 encoding of <paramref name="value" />.
    /// </summary>
    public static RoutingId From(string value)
    {
        if (value == null)
            throw new ArgumentNullException(nameof(value));
        return From(Encoding.UTF8.GetBytes(value));
    }

    /// <summary>
    ///     Creates a routing id by decoding <paramref name="value" /> as a hex
    ///     string (even length, up to 510 hex digits for 255 bytes).
    /// </summary>
    public static RoutingId FromHex(string value)
    {
        if (value == null)
            throw new ArgumentNullException(nameof(value));
        if (value.Length == 0 || (value.Length & 1) != 0)
            throw new ArgumentException(
                "routingId string must be a non-empty even-length hex string.",
                nameof(value));
        if (value.Length > MaxHexStringLength)
            throw new ArgumentOutOfRangeException(nameof(value),
                "routingId string must decode to at most 255 bytes.");

        byte[] bytes;
        try
        {
            bytes = Convert.FromHexString(value);
        }
        catch (FormatException ex)
        {
            throw new ArgumentException(
                "routingId string must contain only hex digits.",
                nameof(value), ex);
        }

        Validate(bytes, nameof(value));
        return new RoutingId(bytes, true);
    }

    /// <summary>
    ///     Creates a 4-byte routing id from <paramref name="value" /> in big-endian
    ///     order.
    /// </summary>
    public static RoutingId From(uint value)
    {
        Span<byte> bytes = stackalloc byte[sizeof(uint)];
        BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
        return new RoutingId(bytes.ToArray(), true);
    }

    /// <summary>
    ///     Creates a 16-byte routing id from <paramref name="value" /> in big-endian
    ///     order.
    /// </summary>
    public static RoutingId From(Guid value)
    {
        var bytes = new byte[16];
        value.TryWriteBytes(bytes, true, out _);
        return new RoutingId(bytes, true);
    }

    /// <summary>
    ///     Gets the length of the routing id in bytes; 0 when empty.
    /// </summary>
    public int Size => _bytes?.Length ?? 0;

    /// <summary>
    ///     Gets whether this routing id has no bytes.
    /// </summary>
    public bool IsEmpty => Size == 0;

    /// <summary>
    ///     Returns the routing id bytes, backed by internal storage; empty when this
    ///     id is empty.
    /// </summary>
    public ReadOnlySpan<byte> ToBytes()
    {
        return _bytes ?? ReadOnlySpan<byte>.Empty;
    }

    /// <summary>
    ///     Returns the routing id as a lowercase hex string.
    /// </summary>
    public string ToHex()
    {
        return Convert.ToHexString(ToBytes()).ToLowerInvariant();
    }

    /// <summary>
    ///     Tries to read a 4-byte routing id as a big-endian <see cref="uint" />.
    /// </summary>
    /// <returns>true when the id is exactly 4 bytes; otherwise false.</returns>
    public bool TryToUInt32(out uint value)
    {
        var bytes = ToBytes();
        if (bytes.Length != sizeof(uint))
        {
            value = 0;
            return false;
        }

        value = BinaryPrimitives.ReadUInt32BigEndian(bytes);
        return true;
    }

    /// <summary>
    ///     Tries to read a 16-byte routing id as a big-endian <see cref="Guid" />.
    /// </summary>
    /// <returns>true when the id is exactly 16 bytes; otherwise false.</returns>
    public bool TryToGuid(out Guid value)
    {
        var bytes = ToBytes();
        if (bytes.Length != 16)
        {
            value = default;
            return false;
        }

        value = new Guid(bytes, true);
        return true;
    }

    /// <summary>
    ///     Returns a human-readable form: printable UTF-8 text when possible,
    ///     otherwise a uint, a Guid, or a "hex:" fallback.
    /// </summary>
    public override string ToString()
    {
        var bytes = ToBytes();
        if (TryToUtf8String(bytes, out var text))
            return text!;
        if (TryToUInt32(out var uint32))
            return uint32.ToString(CultureInfo.InvariantCulture);
        if (TryToGuid(out var guid))
            return guid.ToString("D");
        return "hex:" + ToHex();
    }

    /// <summary>
    ///     Returns true when <paramref name="other" /> has identical bytes.
    /// </summary>
    public bool Equals(RoutingId other)
    {
        if (_hash != other._hash)
            return false;
        return ToBytes().SequenceEqual(other.ToBytes());
    }

    /// <summary>
    ///     Returns true when <paramref name="obj" /> is a routing id with identical
    ///     bytes.
    /// </summary>
    public override bool Equals(object? obj)
    {
        return obj is RoutingId other && Equals(other);
    }

    /// <summary>
    ///     Returns the precomputed hash over the routing id bytes.
    /// </summary>
    public override int GetHashCode()
    {
        return _hash;
    }

    /// <summary>
    ///     Returns true when both routing ids have identical bytes.
    /// </summary>
    public static bool operator ==(RoutingId left, RoutingId right)
    {
        return left.Equals(right);
    }

    /// <summary>
    ///     Returns true when the routing ids differ.
    /// </summary>
    public static bool operator !=(RoutingId left, RoutingId right)
    {
        return !left.Equals(right);
    }

    private static int ComputeHash(ReadOnlySpan<byte> bytes)
    {
        HashCode hash = new();
        for (var i = 0; i < bytes.Length; i++)
            hash.Add(bytes[i]);
        return hash.ToHashCode();
    }

    private static bool TryToUtf8String(ReadOnlySpan<byte> bytes,
        out string? text)
    {
        text = null;
        if (bytes.Length == 0)
            return false;

        var decoded = Encoding.UTF8.GetString(bytes);
        for (var i = 0; i < decoded.Length; i++)
            if (char.IsControl(decoded[i]))
                return false;

        var byteCount = Encoding.UTF8.GetByteCount(decoded);
        if (byteCount != bytes.Length)
            return false;

        var roundtrip = new byte[byteCount];
        Encoding.UTF8.GetBytes(decoded, roundtrip.AsSpan());
        if (!roundtrip.AsSpan().SequenceEqual(bytes))
            return false;

        text = decoded;
        return true;
    }

    private static void Validate(ReadOnlySpan<byte> bytes, string paramName)
    {
        if (bytes.Length <= 0 || bytes.Length > MaxSize)
            throw new ArgumentOutOfRangeException(paramName,
                "routingId length must be between 1 and 255 bytes.");
    }
}
