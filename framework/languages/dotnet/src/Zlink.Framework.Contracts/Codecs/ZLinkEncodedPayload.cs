namespace Zlink.Framework.Contracts.Codecs;

public readonly struct ZLinkEncodedPayload : IEquatable<ZLinkEncodedPayload>
{
    private ZLinkEncodedPayload(ReadOnlyMemory<byte> bytes)
    {
        Bytes = bytes;
    }

    /// <summary>
    /// Bytes owned by this payload value. Public factory methods copy caller
    /// buffers before storing them, so later caller-side mutations do not change
    /// the payload.
    /// </summary>
    public ReadOnlyMemory<byte> Bytes { get; }

    public static ZLinkEncodedPayload From(byte[] bytes)
    {
        ArgumentNullException.ThrowIfNull(bytes);
        return new ZLinkEncodedPayload(bytes.ToArray());
    }

    public static ZLinkEncodedPayload From(ReadOnlyMemory<byte> bytes)
    {
        return new ZLinkEncodedPayload(bytes.ToArray());
    }

    public static ZLinkEncodedPayload From(ReadOnlySpan<byte> bytes)
    {
        return new ZLinkEncodedPayload(bytes.ToArray());
    }

    public byte[] ToArray()
    {
        return Bytes.ToArray();
    }

    public bool Equals(ZLinkEncodedPayload other)
    {
        return Bytes.Span.SequenceEqual(other.Bytes.Span);
    }

    public override bool Equals(object? obj)
    {
        return obj is ZLinkEncodedPayload other && Equals(other);
    }

    public override int GetHashCode()
    {
        var hash = new HashCode();
        foreach (var value in Bytes.Span) hash.Add(value);
        return hash.ToHashCode();
    }

    public static bool operator ==(ZLinkEncodedPayload left, ZLinkEncodedPayload right)
    {
        return left.Equals(right);
    }

    public static bool operator !=(ZLinkEncodedPayload left, ZLinkEncodedPayload right)
    {
        return !left.Equals(right);
    }
}
