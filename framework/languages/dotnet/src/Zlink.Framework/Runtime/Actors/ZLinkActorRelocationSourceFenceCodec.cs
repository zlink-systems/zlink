using System.Buffers.Binary;
using System.Text;

namespace Zlink.Framework.Runtime.Actors;

internal sealed record ZLinkActorRelocationSourceFence(
    string OwnerId,
    ulong OwnerLeaseGeneration,
    RoutingId NodeRid,
    ulong NodeGeneration)
{
    internal ReadOnlyMemory<byte> LegacyRemoteJoinRecovery { get; init; }
}

internal static class ZLinkActorRelocationSourceFenceCodec
{
    private const uint Magic = 0x5a4c5346; // ZLSF
    private const byte Version = 1;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    internal static byte[] Encode(ZLinkActorRelocationSourceFence value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (string.IsNullOrWhiteSpace(value.OwnerId)
            || value.OwnerId.Contains('\0')
            || value.OwnerLeaseGeneration == 0
            || value.NodeRid.IsEmpty
            || value.NodeGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(value));
        var owner = StrictUtf8.GetBytes(value.OwnerId);
        var node = value.NodeRid.ToBytes();
        if (owner.Length > ushort.MaxValue || node.Length > byte.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(value));

        var encoded = new byte[checked(4 + 1 + 2 + owner.Length + 8 + 1
                                       + node.Length + 8)];
        var offset = 0;
        WriteU32(encoded, ref offset, Magic);
        encoded[offset++] = Version;
        WriteU16(encoded, ref offset, checked((ushort)owner.Length));
        owner.CopyTo(encoded.AsSpan(offset));
        offset += owner.Length;
        WriteU64(encoded, ref offset, value.OwnerLeaseGeneration);
        encoded[offset++] = checked((byte)node.Length);
        node.CopyTo(encoded.AsSpan(offset));
        offset += node.Length;
        WriteU64(encoded, ref offset, value.NodeGeneration);
        if (offset != encoded.Length)
            throw new InvalidOperationException(
                "The standalone Actor source fence length is inconsistent.");
        return encoded;
    }

    internal static ZLinkActorRelocationSourceFence Decode(
        ReadOnlySpan<byte> encoded)
    {
        try
        {
            var offset = 0;
            if (ReadU32(encoded, ref offset) != Magic)
                throw new InvalidDataException();
            var version = ReadU8(encoded, ref offset);
            if (version is not (1 or 2))
                throw new InvalidDataException();
            var ownerLength = ReadU16(encoded, ref offset);
            var owner = StrictUtf8.GetString(Read(encoded, ref offset,
                ownerLength));
            var ownerLease = ReadU64(encoded, ref offset);
            var nodeLength = ReadU8(encoded, ref offset);
            var nodeRid = RoutingId.From(Read(encoded, ref offset, nodeLength));
            var nodeGeneration = ReadU64(encoded, ref offset);
            ReadOnlyMemory<byte> legacyRemoteJoinRecovery = default;
            if (version == 2)
            {
                var legacyRecoveryLength =
                    checked((int)ReadU32(encoded, ref offset));
                if (legacyRecoveryLength > 1024 * 1024)
                    throw new InvalidDataException();
                legacyRemoteJoinRecovery =
                    Read(encoded, ref offset, legacyRecoveryLength).ToArray();
            }
            if (offset != encoded.Length
                || string.IsNullOrWhiteSpace(owner)
                || owner.Contains('\0')
                || ownerLease == 0
                || nodeRid.IsEmpty
                || nodeGeneration == 0)
                throw new InvalidDataException();
            return new ZLinkActorRelocationSourceFence(
                owner, ownerLease, nodeRid, nodeGeneration)
            {
                LegacyRemoteJoinRecovery = legacyRemoteJoinRecovery
            };
        }
        catch (Exception exception) when (exception is ArgumentException
                                          or DecoderFallbackException
                                          or OverflowException
                                          or IndexOutOfRangeException)
        {
            throw new InvalidDataException(
                "The standalone Actor source fence is malformed.",
                exception);
        }
    }

    private static ReadOnlySpan<byte> Read(
        ReadOnlySpan<byte> source,
        ref int offset,
        int length)
    {
        if (length < 0 || offset > source.Length - length)
            throw new InvalidDataException();
        var value = source.Slice(offset, length);
        offset += length;
        return value;
    }

    private static byte ReadU8(ReadOnlySpan<byte> source, ref int offset)
        => Read(source, ref offset, 1)[0];

    private static ushort ReadU16(ReadOnlySpan<byte> source, ref int offset)
        => BinaryPrimitives.ReadUInt16BigEndian(Read(source, ref offset, 2));

    private static uint ReadU32(ReadOnlySpan<byte> source, ref int offset)
        => BinaryPrimitives.ReadUInt32BigEndian(Read(source, ref offset, 4));

    private static ulong ReadU64(ReadOnlySpan<byte> source, ref int offset)
        => BinaryPrimitives.ReadUInt64BigEndian(Read(source, ref offset, 8));

    private static void WriteU16(Span<byte> target, ref int offset, ushort value)
    {
        BinaryPrimitives.WriteUInt16BigEndian(target[offset..], value);
        offset += 2;
    }

    private static void WriteU32(Span<byte> target, ref int offset, uint value)
    {
        BinaryPrimitives.WriteUInt32BigEndian(target[offset..], value);
        offset += 4;
    }

    private static void WriteU64(Span<byte> target, ref int offset, ulong value)
    {
        BinaryPrimitives.WriteUInt64BigEndian(target[offset..], value);
        offset += 8;
    }
}
