using System.Buffers.Binary;
using System.Text;

namespace Zlink.Framework.Runtime.Locations;

internal sealed record ZLinkCanonicalParticipantRecovery(
    ZLinkAuthorityKey AuthorityKey,
    ZLinkPlacementObjectKind ObjectKind,
    ulong ObjectGeneration,
    ulong AuthorityOwnerGeneration,
    string ExpectedStoreVersion,
    string StableType,
    ReadOnlyMemory<byte> AuthorityPayload,
    ReadOnlyMemory<byte> MembershipMutation,
    ReadOnlyMemory<byte> OperationRecovery = default,
    ZLinkObjectMaintenancePolicyKind MaintenancePolicy =
        ZLinkObjectMaintenancePolicyKind.Snapshot);

internal static class ZLinkCanonicalParticipantRecoveryCodec
{
    private const uint Magic = 0x5a4c5250; // ZLRP
    private const byte Version = 3;
    private const int MaximumFieldBytes = 1024 * 1024;
    private const int MaximumOperationRecoveryBytes =
        2 * 1024 * 1024 + 256 * 1024 + 64;
    // Current Spot relocation recovery records keep MembershipMutation empty.
    // The three Text16 fields can each contain ushort.MaxValue UTF-8 bytes.
    internal const int MaximumEncodedBytesWithEmptyMembership =
        sizeof(uint) + sizeof(byte)
        + 3 * (sizeof(ushort) + ushort.MaxValue)
        + sizeof(byte) + 2 * sizeof(ulong)
        + sizeof(uint) + MaximumFieldBytes
        + sizeof(uint)
        + sizeof(uint)
        + sizeof(byte);
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    internal static byte[] Encode(ZLinkCanonicalParticipantRecovery value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (!Enum.IsDefined(value.ObjectKind)
            || value.ObjectGeneration == 0
            || value.AuthorityOwnerGeneration == 0
            || value.MaintenancePolicy
               == ZLinkObjectMaintenancePolicyKind.Unspecified
            || !Enum.IsDefined(value.MaintenancePolicy)
            || value.AuthorityPayload.Length > MaximumFieldBytes
            || value.MembershipMutation.Length > MaximumFieldBytes
            || value.OperationRecovery.Length > MaximumOperationRecoveryBytes)
            throw new ArgumentOutOfRangeException(nameof(value));
        using var stream = new MemoryStream();
        U32(stream, Magic);
        stream.WriteByte(Version);
        Text16(stream, value.AuthorityKey.Value);
        stream.WriteByte((byte)value.ObjectKind);
        U64(stream, value.ObjectGeneration);
        U64(stream, value.AuthorityOwnerGeneration);
        Text16(stream, value.ExpectedStoreVersion);
        Text16(stream, value.StableType);
        Bytes32(stream, value.AuthorityPayload.Span);
        Bytes32(stream, value.MembershipMutation.Span);
        Bytes32(
            stream,
            value.OperationRecovery.Span,
            MaximumOperationRecoveryBytes);
        stream.WriteByte((byte)value.MaintenancePolicy);
        return stream.ToArray();
    }

    internal static ZLinkCanonicalParticipantRecovery Decode(
        ReadOnlySpan<byte> encoded)
    {
        var reader = new Reader(encoded);
        if (reader.U32() != Magic)
            throw new InvalidDataException(
                "The canonical participant recovery header is invalid.");
        var version = reader.U8();
        if (version is not (1 or 2 or Version))
            throw new InvalidDataException(
                "The canonical participant recovery version is invalid.");
        var key = new ZLinkAuthorityKey(reader.Text16());
        var kind = (ZLinkPlacementObjectKind)reader.U8();
        var objectGeneration = reader.U64();
        var ownerGeneration = reader.U64();
        var storeVersion = reader.Text16();
        var stableType = reader.Text16();
        var authorityPayload = reader.Bytes32();
        var membershipMutation = reader.Bytes32();
        var operationRecovery = version == 1
            ? ReadOnlyMemory<byte>.Empty
            : reader.Bytes32(MaximumOperationRecoveryBytes);
        var maintenancePolicy = version < Version
            ? ZLinkObjectMaintenancePolicyKind.Unspecified
            : (ZLinkObjectMaintenancePolicyKind)reader.U8();
        if (!reader.End || !Enum.IsDefined(kind)
            || !Enum.IsDefined(maintenancePolicy)
            || version == Version
            && maintenancePolicy
               == ZLinkObjectMaintenancePolicyKind.Unspecified
            || objectGeneration == 0 || ownerGeneration == 0
            || string.IsNullOrWhiteSpace(key.Value)
            || string.IsNullOrWhiteSpace(storeVersion)
            || string.IsNullOrWhiteSpace(stableType))
            throw new InvalidDataException(
                "The canonical participant recovery record is invalid.");
        return new ZLinkCanonicalParticipantRecovery(
            key, kind, objectGeneration, ownerGeneration, storeVersion,
            stableType, authorityPayload, membershipMutation,
            operationRecovery, maintenancePolicy);
    }

    internal static bool IsEncoded(ReadOnlySpan<byte> encoded)
    {
        try
        {
            _ = Decode(encoded);
            return true;
        }
        catch (Exception error) when (error is InvalidDataException
                                      or EndOfStreamException
                                      or DecoderFallbackException
                                      or ArgumentException
                                      or OverflowException)
        {
            return false;
        }
    }

    private static void Text16(Stream stream, string value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value);
        var bytes = StrictUtf8.GetBytes(value);
        if (bytes.Length > ushort.MaxValue || value.Contains('\0'))
            throw new ArgumentOutOfRangeException(nameof(value));
        U16(stream, checked((ushort)bytes.Length));
        stream.Write(bytes);
    }

    private static void Bytes32(
        Stream stream,
        ReadOnlySpan<byte> value,
        int maximum = MaximumFieldBytes)
    {
        if (value.Length > maximum)
            throw new ArgumentOutOfRangeException(nameof(value));
        U32(stream, checked((uint)value.Length));
        stream.Write(value);
    }

    private static void U16(Stream stream, ushort value)
    {
        Span<byte> bytes = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private static void U32(Stream stream, uint value)
    {
        Span<byte> bytes = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private static void U64(Stream stream, ulong value)
    {
        Span<byte> bytes = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private ref struct Reader(ReadOnlySpan<byte> value)
    {
        private ReadOnlySpan<byte> _value = value;
        private int _offset;
        internal bool End => _offset == _value.Length;
        internal byte U8() => Slice(1)[0];
        internal ushort U16() => BinaryPrimitives.ReadUInt16BigEndian(Slice(2));
        internal uint U32() => BinaryPrimitives.ReadUInt32BigEndian(Slice(4));
        internal ulong U64() => BinaryPrimitives.ReadUInt64BigEndian(Slice(8));
        internal string Text16()
        {
            var bytes = Slice(U16());
            var result = StrictUtf8.GetString(bytes);
            if (result.Contains('\0')) throw new InvalidDataException();
            return result;
        }
        internal byte[] Bytes32(int maximum = MaximumFieldBytes)
        {
            var length = U32();
            if (length > maximum) throw new InvalidDataException();
            return Slice(checked((int)length)).ToArray();
        }
        private ReadOnlySpan<byte> Slice(int length)
        {
            if (length < 0 || _value.Length - _offset < length)
                throw new EndOfStreamException();
            var result = _value.Slice(_offset, length);
            _offset += length;
            return result;
        }
    }
}
