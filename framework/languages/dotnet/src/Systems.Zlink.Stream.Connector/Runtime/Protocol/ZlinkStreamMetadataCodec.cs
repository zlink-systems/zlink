using System.Buffers.Binary;
using System.Text;

namespace Systems.Zlink.Stream.Connector.Runtime.Protocol;

internal static class ZlinkStreamMetadataCodec
{
    public static int GetPayloadSize(ZlinkStreamMetadata metadata)
    {
        return metadata.Count == 0 ? 0 : CalculatePayloadSize(metadata);
    }

    public static void Write(ZlinkStreamMetadata metadata, Span<byte> destination)
    {
        var offset = 0;
        destination[offset++] = (byte)metadata.Count;
        foreach (var (key, value) in metadata.Values)
        {
            var keyLengthOffset = offset++;
            var keyLength = Encoding.UTF8.GetBytes(key, destination[offset..]);
            destination[keyLengthOffset] = checked((byte)keyLength);
            offset += keyLength;

            var valueLengthOffset = offset;
            offset += 2;
            var valueLength = Encoding.UTF8.GetBytes(value, destination[offset..]);
            BinaryPrimitives.WriteUInt16BigEndian(
                destination.Slice(valueLengthOffset, 2),
                checked((ushort)valueLength));
            offset += valueLength;
        }
    }

    public static ZlinkStreamMetadata Decode(ReadOnlySpan<byte> metadata)
    {
        if (metadata.Length == 0)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed, "Metadata payload is empty.");

        var offset = 0;
        var count = metadata[offset++];
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        for (var i = 0; i < count; i++)
        {
            var key = DecodeString(metadata, ref offset, true, "key");
            var value = DecodeString(metadata, ref offset, false, "value", allowEmpty: true);

            if (!values.TryAdd(key, value))
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed, "Duplicate metadata key.");
        }

        if (offset != metadata.Length)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                "Metadata contains trailing bytes.");

        return ZlinkStreamMetadata.FromDictionary(values);
    }

    private static int CalculatePayloadSize(ZlinkStreamMetadata metadata)
    {
        if (metadata.Count > byte.MaxValue)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.ValidationFailed,
                "Metadata entry count must not exceed 255.");

        var size = 1;
        foreach (var (key, value) in metadata.Values)
        {
            var keyLength = Encoding.UTF8.GetByteCount(key);
            var valueLength = Encoding.UTF8.GetByteCount(value);
            if (keyLength is 0 or > byte.MaxValue)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.ValidationFailed,
                    "Metadata key length is invalid.");

            if (valueLength > ushort.MaxValue)
                throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.ValidationFailed, "Metadata value is too large.");

            size = checked(size + 1 + keyLength + 2 + valueLength);
        }

        return size;
    }

    private static string DecodeString(
        ReadOnlySpan<byte> metadata,
        ref int offset,
        bool byteLength,
        string name,
        bool allowEmpty = false)
    {
        var length = byteLength
            ? ReadByteLength(metadata, ref offset, name)
            : ReadUInt16Length(metadata, ref offset, name);
        if ((!allowEmpty && length == 0) || metadata.Length - offset < length)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed, $"Metadata {name} is invalid.");

        var value = Encoding.UTF8.GetString(metadata.Slice(offset, length));
        offset += length;
        return value;
    }

    private static int ReadByteLength(ReadOnlySpan<byte> metadata, ref int offset, string name)
    {
        if (metadata.Length - offset < 1)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                $"Metadata {name} length is missing.");

        return metadata[offset++];
    }

    private static int ReadUInt16Length(ReadOnlySpan<byte> metadata, ref int offset, string name)
    {
        if (metadata.Length - offset < 2)
            throw ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed,
                $"Metadata {name} length is missing.");

        var length = BinaryPrimitives.ReadUInt16BigEndian(metadata.Slice(offset, 2));
        offset += 2;
        return length;
    }
}
