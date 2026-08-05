using System.Buffers.Binary;
using System.Text;

namespace Zlink.Framework.Runtime.Spots;

internal enum ZLinkUserSpotAuthorityState : byte
{
    Creating = 0,
    Ready = 1,
    Closing = 2
}

internal sealed record ZLinkUserSpotAuthorityPayload(
    ZLinkUserSpotAuthorityState State,
    string StableType,
    string SpotId,
    string OwnerId,
    ulong OwnerLeaseGeneration,
    string MeshName,
    RoutingId NodeRid,
    ulong NodeGeneration);

internal static class ZLinkUserSpotAuthorityPayloadCodec
{
    private static ReadOnlySpan<byte> Magic => "ZLAU"u8;
    private const byte Version = 1;
    private const int MaximumBytes = 1024 * 1024;

    internal static byte[] Encode(ZLinkUserSpotAuthorityPayload value)
    {
        var spot = new Writer();
        spot.Text8(value.SpotId);
        spot.Text8(value.StableType);
        spot.U8((byte)value.State);
        var identity = new Writer();
        identity.U8(2);
        identity.U16(checked((ushort)spot.Count));
        identity.Bytes(spot.ToArray());

        var body = new Writer();
        body.U8(value.State switch
        {
            ZLinkUserSpotAuthorityState.Creating => 1,
            ZLinkUserSpotAuthorityState.Ready => 0,
            ZLinkUserSpotAuthorityState.Closing => 3,
            _ => throw new ArgumentOutOfRangeException(nameof(value))
        });
        body.U8(2);
        body.U16(checked((ushort)identity.Count));
        body.Bytes(identity.ToArray());
        body.Text8(value.OwnerId);
        body.U64(value.OwnerLeaseGeneration);
        body.Text8(value.MeshName);
        body.Rid(value.NodeRid);
        body.U64(value.NodeGeneration);
        body.U8(0);
        body.U32(0);
        body.U8(0);
        body.U32(0);

        var result = new Writer();
        result.Bytes(Magic);
        result.U8(Version);
        result.U16(0);
        result.U32(checked((uint)body.Count));
        result.Bytes(body.ToArray());
        result.U32(Zlink.Framework.Runtime.Locations.ZLinkCrc32C.Compute(
            result.WrittenSpan));
        var encoded = result.ToArray();
        if (encoded.Length > MaximumBytes)
            throw new ArgumentOutOfRangeException(nameof(value));
        return encoded;
    }

    internal static bool TryDecode(
        ReadOnlySpan<byte> encoded,
        out ZLinkUserSpotAuthorityPayload value)
    {
        value = null!;
        try
        {
            if (encoded.Length > MaximumBytes || encoded.Length < 15
                || !encoded[..4].SequenceEqual(Magic))
                return false;
            var reader = new Reader(encoded);
            reader.Skip(4);
            if (reader.U8() != Version || reader.U16() != 0)
                return false;
            var body = reader.Slice(checked((int)reader.U32()));
            var checksumOffset = reader.Offset;
            if (reader.U32()
                != Zlink.Framework.Runtime.Locations.ZLinkCrc32C.Compute(
                    encoded[..checksumOffset])
                || !reader.End)
                return false;
            var operation = body.U8();
            if (body.U8() != 2)
                return false;
            var identity = body.Slice(body.U16());
            if (identity.U8() != 2)
                return false;
            var spot = identity.Slice(identity.U16());
            var spotId = spot.Text8();
            var stableType = spot.Text8();
            var state = (ZLinkUserSpotAuthorityState)spot.U8();
            if (!spot.End || !identity.End
                || state switch
                {
                    ZLinkUserSpotAuthorityState.Creating => operation != 1,
                    ZLinkUserSpotAuthorityState.Ready => operation != 0,
                    ZLinkUserSpotAuthorityState.Closing => operation != 3,
                    _ => true
                })
                return false;
            var ownerId = body.Text8();
            var ownerLease = body.U64();
            var meshName = body.Text8();
            var nodeRid = body.Rid();
            var nodeGeneration = body.U64();
            if (ownerLease == 0 || nodeGeneration == 0
                || body.U8() != 0 || body.U32() != 0
                || body.U8() != 0 || body.U32() != 0 || !body.End)
                return false;
            value = new ZLinkUserSpotAuthorityPayload(
                state, stableType, spotId, ownerId, ownerLease,
                meshName, nodeRid, nodeGeneration);
            return true;
        }
        catch (Exception error) when (error is InvalidDataException
                                      or OverflowException
                                      or DecoderFallbackException)
        {
            return false;
        }
    }

    internal static ZLinkAuthorityKey AuthorityKey(string spotId)
    {
        var bytes = new UTF8Encoding(false, true).GetBytes(
            ZLinkSpotId.Require(spotId, nameof(spotId)));
        if (bytes.Length is 0 or > byte.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(spotId));
        var builder = new StringBuilder($"zla1:s:{bytes.Length}:");
        foreach (var item in bytes)
        {
            if (item is >= (byte)'A' and <= (byte)'Z'
                or >= (byte)'a' and <= (byte)'z'
                or >= (byte)'0' and <= (byte)'9'
                or (byte)'-' or (byte)'.' or (byte)'_' or (byte)'~')
                builder.Append((char)item);
            else
                builder.Append('%').Append(item.ToString("X2"));
        }
        return new ZLinkAuthorityKey(builder.ToString());
    }

    internal static bool TryGetSpotId(
        ZLinkAuthorityKey key,
        out string spotId)
    {
        spotId = string.Empty;
        const string prefix = "zla1:s:";
        if (!key.Value.StartsWith(prefix, StringComparison.Ordinal))
            return false;
        var lengthEnd = key.Value.IndexOf(':', prefix.Length);
        if (lengthEnd < 0
            || !int.TryParse(
                key.Value.AsSpan(prefix.Length, lengthEnd - prefix.Length),
                out var expectedLength)
            || expectedLength is < 1 or > byte.MaxValue)
            return false;

        var encoded = key.Value.AsSpan(lengthEnd + 1);
        var bytes = new List<byte>(expectedLength);
        for (var index = 0; index < encoded.Length;)
        {
            if (encoded[index] != '%')
            {
                if (encoded[index] > 0x7f)
                    return false;
                bytes.Add((byte)encoded[index++]);
                continue;
            }
            if (index + 2 >= encoded.Length
                || !byte.TryParse(
                    encoded.Slice(index + 1, 2),
                    System.Globalization.NumberStyles.HexNumber,
                    System.Globalization.CultureInfo.InvariantCulture,
                    out var value))
                return false;
            bytes.Add(value);
            index += 3;
        }
        if (bytes.Count != expectedLength)
            return false;
        try
        {
            spotId = new UTF8Encoding(false, true).GetString([.. bytes]);
            return ZLinkSpotId.IsValid(spotId);
        }
        catch (DecoderFallbackException)
        {
            spotId = string.Empty;
            return false;
        }
    }

    private sealed class Writer
    {
        private readonly MemoryStream _stream = new();
        internal int Count => checked((int)_stream.Length);
        internal ReadOnlySpan<byte> WrittenSpan => _stream.GetBuffer().AsSpan(0, Count);
        internal void U8(byte value) => _stream.WriteByte(value);
        internal void U16(ushort value)
        {
            Span<byte> bytes = stackalloc byte[2];
            BinaryPrimitives.WriteUInt16BigEndian(bytes, value);
            _stream.Write(bytes);
        }
        internal void U32(uint value)
        {
            Span<byte> bytes = stackalloc byte[4];
            BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
            _stream.Write(bytes);
        }
        internal void U64(ulong value)
        {
            if (value == 0) throw new ArgumentOutOfRangeException(nameof(value));
            Span<byte> bytes = stackalloc byte[8];
            BinaryPrimitives.WriteUInt64BigEndian(bytes, value);
            _stream.Write(bytes);
        }
        internal void Text8(string value)
        {
            var bytes = new UTF8Encoding(false, true).GetBytes(value);
            if (bytes.Length is 0 or > byte.MaxValue || value.Contains('\0'))
                throw new ArgumentOutOfRangeException(nameof(value));
            U8(checked((byte)bytes.Length));
            Bytes(bytes);
        }
        internal void Rid(RoutingId value)
        {
            var bytes = value.ToBytes();
            if (bytes.Length is 0 or > byte.MaxValue)
                throw new ArgumentOutOfRangeException(nameof(value));
            U8(checked((byte)bytes.Length));
            Bytes(bytes);
        }
        internal void Bytes(ReadOnlySpan<byte> value) => _stream.Write(value);
        internal byte[] ToArray() => _stream.ToArray();
    }

    private ref struct Reader(ReadOnlySpan<byte> bytes)
    {
        private readonly ReadOnlySpan<byte> _bytes = bytes;
        internal int Offset { get; private set; }
        internal bool End => Offset == _bytes.Length;
        internal void Skip(int count)
        {
            Require(count);
            Offset += count;
        }
        internal byte U8()
        {
            Require(1);
            return _bytes[Offset++];
        }
        internal ushort U16()
        {
            Require(2);
            var value = BinaryPrimitives.ReadUInt16BigEndian(_bytes[Offset..]);
            Offset += 2;
            return value;
        }
        internal uint U32()
        {
            Require(4);
            var value = BinaryPrimitives.ReadUInt32BigEndian(_bytes[Offset..]);
            Offset += 4;
            return value;
        }
        internal ulong U64()
        {
            Require(8);
            var value = BinaryPrimitives.ReadUInt64BigEndian(_bytes[Offset..]);
            Offset += 8;
            return value;
        }
        internal Reader Slice(int count)
        {
            Require(count);
            var value = new Reader(_bytes.Slice(Offset, count));
            Offset += count;
            return value;
        }
        internal string Text8()
        {
            var length = U8();
            var value = new UTF8Encoding(false, true)
                .GetString(Slice(length)._bytes);
            if (value.Length == 0 || value.Contains('\0'))
                throw new InvalidDataException();
            return value;
        }
        internal RoutingId Rid()
        {
            var length = U8();
            if (length == 0) throw new InvalidDataException();
            return RoutingId.From(Slice(length)._bytes);
        }
        private void Require(int count)
        {
            if (count < 0 || _bytes.Length - Offset < count)
                throw new InvalidDataException();
        }
    }
}
