using System.Buffers.Binary;
using System.Diagnostics.CodeAnalysis;
using System.Globalization;
using System.Text;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Spots;

internal readonly record struct ZLinkApplicationPayloadEnvelope(
    string PacketName,
    string ContentType,
    ReadOnlyMemory<byte> Payload);

internal static class ZLinkApplicationPayloadEnvelopeCodec
{
    private const byte Version = 1;
    internal const string CreationPacketName = "ZLinkFrameworkCreationRequest";
    private const string MultipartPacketName =
        Systems.Zlink.Framework.Runtime.Protocol.ServiceWireConstants.FrameworkMultipartPacketName;
    private const string MultipartContentType =
        Systems.Zlink.Framework.Runtime.Protocol.ServiceWireConstants.FrameworkMultipartContentType;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private static readonly byte[] MultipartPacketNameUtf8 =
        StrictUtf8.GetBytes(MultipartPacketName);
    private static readonly byte[] MultipartContentTypeUtf8 =
        StrictUtf8.GetBytes(MultipartContentType);

    internal static byte[] Encode(
        string packetName,
        string contentType,
        ReadOnlySpan<byte> payload)
    {
        ArgumentException.ThrowIfNullOrEmpty(packetName);
        ArgumentException.ThrowIfNullOrEmpty(contentType);
        if (packetName.Contains('\0') || contentType.Contains('\0'))
            throw new ArgumentException(
                "Application payload text fields cannot contain NUL.");
        var packetLength = StrictUtf8.GetByteCount(packetName);
        var typeLength = StrictUtf8.GetByteCount(contentType);
        if (packetLength > byte.MaxValue || typeLength > byte.MaxValue)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "Application payload text fields must fit in 255 UTF-8 bytes.");
        var bodyLength = checked(1 + packetLength + 1 + typeLength + 4 + payload.Length);
        var result = new byte[checked(1 + 4 + bodyLength)];
        var offset = 0;
        result[offset++] = Version;
        BinaryPrimitives.WriteUInt32BigEndian(
            result.AsSpan(offset, 4),
            checked((uint)bodyLength));
        offset += 4;
        result[offset++] = checked((byte)packetLength);
        offset += StrictUtf8.GetBytes(
            packetName,
            result.AsSpan(offset, packetLength));
        result[offset++] = checked((byte)typeLength);
        offset += StrictUtf8.GetBytes(
            contentType,
            result.AsSpan(offset, typeLength));
        BinaryPrimitives.WriteUInt32BigEndian(
            result.AsSpan(offset, 4),
            checked((uint)payload.Length));
        offset += 4;
        payload.CopyTo(result.AsSpan(offset));
        return result;
    }

    internal static bool TryDecode(
        ReadOnlyMemory<byte> frame,
        out ZLinkApplicationPayloadEnvelope envelope)
    {
        envelope = default;
        var span = frame.Span;
        if (span.Length < 11 || span[0] != Version)
            return false;
        var bodyLength = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(1, 4));
        if (bodyLength != span.Length - 5)
            return false;
        var offset = 5;
        var packetLength = span[offset++];
        if (packetLength == 0)
            return false;
        if (span.Length - offset < packetLength + 1)
            return false;
        string packetName;
        try
        {
            packetName = StrictUtf8.GetString(span.Slice(offset, packetLength));
        }
        catch (DecoderFallbackException)
        {
            return false;
        }
        offset += packetLength;
        var typeLength = span[offset++];
        if (typeLength == 0)
            return false;
        if (span.Length - offset < typeLength + 4)
            return false;
        string contentType;
        try
        {
            contentType = StrictUtf8.GetString(span.Slice(offset, typeLength));
        }
        catch (DecoderFallbackException)
        {
            return false;
        }
        offset += typeLength;
        var payloadLength = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(offset, 4));
        offset += 4;
        if (payloadLength != span.Length - offset)
            return false;
        envelope = new ZLinkApplicationPayloadEnvelope(
            packetName,
            contentType,
            frame.Slice(offset));
        return true;
    }

    internal static byte[] EncodeFrameworkMultipart(
        IReadOnlyList<Message> parts)
    {
        ArgumentNullException.ThrowIfNull(parts);
        return EncodeFrameworkMultipartCore(parts);
    }

    internal static byte[] EncodeFrameworkMultipart(
        IReadOnlyList<ReadOnlyMemory<byte>> parts)
    {
        ArgumentNullException.ThrowIfNull(parts);
        return EncodeFrameworkMultipartCore(parts);
    }

    internal static Message EncodeFrameworkMultipartMessage(
        IReadOnlyList<Message> parts)
    {
        ArgumentNullException.ThrowIfNull(parts);
        var encodedLength = GetFrameworkMultipartEncodedLength(parts);
        EnsureRepresentableEncodedLength(encodedLength);
        var result = Message.Allocate(checked((int)encodedLength));
        try
        {
            WriteFrameworkMultipartEnvelope(
                result.AsSpan(),
                parts,
                encodedLength);
            return result;
        }
        catch
        {
            result.Dispose();
            throw;
        }
    }

    internal static long GetFrameworkMultipartEncodedLength(
        IReadOnlyList<Message> parts)
    {
        ArgumentNullException.ThrowIfNull(parts);
        return GetEnvelopeLength(GetMultipartPayloadLength(parts));
    }

    internal static long GetFrameworkMultipartEncodedLength(
        IReadOnlyList<ReadOnlyMemory<byte>> parts)
    {
        ArgumentNullException.ThrowIfNull(parts);
        return GetEnvelopeLength(GetMultipartPayloadLength(parts));
    }

    internal static bool TryDecodeFrameworkMultipart(
        ReadOnlyMemory<byte> frame,
        out Message[] parts)
    {
        parts = [];
        if (!TryDecode(frame, out var envelope)
            || !string.Equals(
                envelope.PacketName,
                MultipartPacketName,
                StringComparison.Ordinal)
            || !string.Equals(
                envelope.ContentType,
                MultipartContentType,
                StringComparison.Ordinal))
            return false;

        return TryDecodeMultipart(envelope.Payload.Span, out parts);
    }

    internal static bool TryDecodeFrameworkMultipart(
        Message frame,
        out Message[] parts)
    {
        ArgumentNullException.ThrowIfNull(frame);
        parts = [];
        var span = frame.AsReadOnlySpan();
        if (!TryGetFrameworkMultipartPayloadOffset(span, out var offset))
            return false;
        return TryDecodeMultipart(span.Slice(offset), out parts);
    }

    internal static bool TryDecodeFrameworkMultipartView(
        Message frame,
        [NotNullWhen(true)] out ZLinkMultipartPayloadView? parts)
    {
        ArgumentNullException.ThrowIfNull(frame);
        parts = null;
        var span = frame.AsReadOnlySpan();
        if (!TryGetFrameworkMultipartPayloadOffset(
                span,
                out var offset))
            return false;
        return TryDecodeMultipartView(frame, span, offset, out parts);
    }

    private static bool TryGetFrameworkMultipartPayloadOffset(
        ReadOnlySpan<byte> span,
        out int offset)
    {
        offset = 0;
        if (span.Length < 11 || span[0] != Version)
            return false;
        var bodyLength = BinaryPrimitives.ReadUInt32BigEndian(
            span.Slice(1, sizeof(uint)));
        if (bodyLength != span.Length - 1 - sizeof(uint))
            return false;

        offset = 1 + sizeof(uint);
        var packetLength = span[offset++];
        if (packetLength != MultipartPacketNameUtf8.Length
            || span.Length - offset < packetLength + 1
            || !span.Slice(offset, packetLength)
                .SequenceEqual(MultipartPacketNameUtf8))
            return false;
        offset += packetLength;

        var typeLength = span[offset++];
        if (typeLength != MultipartContentTypeUtf8.Length
            || span.Length - offset < typeLength + sizeof(uint)
            || !span.Slice(offset, typeLength)
                .SequenceEqual(MultipartContentTypeUtf8))
            return false;
        offset += typeLength;

        var payloadLength = BinaryPrimitives.ReadUInt32BigEndian(
            span.Slice(offset, sizeof(uint)));
        offset += sizeof(uint);
        return payloadLength == span.Length - offset;
    }

    private static byte[] EncodeFrameworkMultipartCore(
        IReadOnlyList<Message> parts)
    {
        var encodedLength = GetFrameworkMultipartEncodedLength(parts);
        EnsureRepresentableEncodedLength(encodedLength);
        var result = new byte[checked((int)encodedLength)];
        WriteFrameworkMultipartEnvelope(result, parts, encodedLength);
        return result;
    }

    private static byte[] EncodeFrameworkMultipartCore(
        IReadOnlyList<ReadOnlyMemory<byte>> parts)
    {
        var encodedLength = GetFrameworkMultipartEncodedLength(parts);
        EnsureRepresentableEncodedLength(encodedLength);
        var result = new byte[checked((int)encodedLength)];
        WriteFrameworkMultipartEnvelope(result, parts, encodedLength);
        return result;
    }

    private static long GetMultipartPayloadLength(
        IReadOnlyList<Message> parts)
    {
        EnsureParts(parts);
        long size = sizeof(uint);
        for (var index = 0; index < parts.Count; index++)
            size = checked(size + sizeof(uint) + parts[index].Size);
        return size;
    }

    private static long GetMultipartPayloadLength(
        IReadOnlyList<ReadOnlyMemory<byte>> parts)
    {
        EnsureParts(parts);
        long size = sizeof(uint);
        for (var index = 0; index < parts.Count; index++)
            size = checked(size + sizeof(uint) + parts[index].Length);
        return size;
    }

    private static long GetEnvelopeLength(long multipartPayloadLength)
    {
        var bodyLength = checked(
            1 + MultipartPacketNameUtf8.Length
            + 1 + MultipartContentTypeUtf8.Length
            + sizeof(uint) + multipartPayloadLength);
        return checked(1 + sizeof(uint) + bodyLength);
    }

    private static void EnsureParts<T>(IReadOnlyList<T> parts)
    {
        if (parts.Count == 0)
            throw new ArgumentException(
                "Framework multipart payload must contain at least one part.",
                nameof(parts));
    }

    private static void EnsureRepresentableEncodedLength(long encodedLength)
    {
        if (encodedLength > int.MaxValue)
            throw new ArgumentOutOfRangeException(
                "parts",
                "Framework multipart payload cannot be represented by one .NET byte array.");
    }

    private static void WriteFrameworkMultipartEnvelope(
        Span<byte> result,
        IReadOnlyList<Message> parts,
        long encodedLength)
    {
        WriteEnvelopeHeader(result, checked((int)encodedLength));
        var offset = 1 + sizeof(uint);
        var payloadOffset = WriteMultipartEnvelopeFields(
            result,
            offset,
            checked((uint)GetMultipartPayloadLength(parts)));
        WriteMultipartPayload(result, payloadOffset, parts);
    }

    private static void WriteFrameworkMultipartEnvelope(
        Span<byte> result,
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        long encodedLength)
    {
        WriteEnvelopeHeader(result, checked((int)encodedLength));
        var offset = 1 + sizeof(uint);
        var payloadOffset = WriteMultipartEnvelopeFields(
            result,
            offset,
            checked((uint)GetMultipartPayloadLength(parts)));
        WriteMultipartPayload(result, payloadOffset, parts);
    }

    private static void WriteEnvelopeHeader(Span<byte> result, int encodedLength)
    {
        result[0] = Version;
        BinaryPrimitives.WriteUInt32BigEndian(
            result.Slice(1, sizeof(uint)),
            checked((uint)(encodedLength - 1 - sizeof(uint))));
    }

    private static int WriteMultipartEnvelopeFields(
        Span<byte> result,
        int offset,
        uint payloadLength)
    {
        result[offset++] = checked((byte)MultipartPacketNameUtf8.Length);
        MultipartPacketNameUtf8.CopyTo(result.Slice(offset));
        offset += MultipartPacketNameUtf8.Length;
        result[offset++] = checked((byte)MultipartContentTypeUtf8.Length);
        MultipartContentTypeUtf8.CopyTo(result.Slice(offset));
        offset += MultipartContentTypeUtf8.Length;
        BinaryPrimitives.WriteUInt32BigEndian(
            result.Slice(offset, sizeof(uint)),
            payloadLength);
        return offset + sizeof(uint);
    }

    private static void WriteMultipartPayload(
        Span<byte> result,
        int offset,
        IReadOnlyList<Message> parts)
    {
        BinaryPrimitives.WriteUInt32BigEndian(
            result.Slice(offset, sizeof(uint)),
            checked((uint)parts.Count));
        offset += sizeof(uint);
        for (var index = 0; index < parts.Count; index++)
        {
            var part = parts[index];
            BinaryPrimitives.WriteUInt32BigEndian(
                result.Slice(offset, sizeof(uint)),
                checked((uint)part.Size));
            offset += sizeof(uint);
            part.AsReadOnlySpan().CopyTo(result.Slice(offset, part.Size));
            offset += part.Size;
        }
    }

    private static void WriteMultipartPayload(
        Span<byte> result,
        int offset,
        IReadOnlyList<ReadOnlyMemory<byte>> parts)
    {
        BinaryPrimitives.WriteUInt32BigEndian(
            result.Slice(offset, sizeof(uint)),
            checked((uint)parts.Count));
        offset += sizeof(uint);
        for (var index = 0; index < parts.Count; index++)
        {
            var part = parts[index];
            BinaryPrimitives.WriteUInt32BigEndian(
                result.Slice(offset, sizeof(uint)),
                checked((uint)part.Length));
            offset += sizeof(uint);
            part.Span.CopyTo(result.Slice(offset, part.Length));
            offset += part.Length;
        }
    }

    private static bool TryDecodeMultipart(
        ReadOnlySpan<byte> span,
        out Message[] parts)
    {
        parts = [];
        if (span.Length < sizeof(uint))
            return false;

        var count = BinaryPrimitives.ReadUInt32BigEndian(span);
        if (count == 0
            || count > int.MaxValue
            || count > (span.Length - sizeof(uint)) / sizeof(uint))
            return false;

        var partCount = checked((int)count);
        var offset = sizeof(uint);
        for (var index = 0; index < partCount; index++)
        {
            if (span.Length - offset < sizeof(uint))
                return false;
            var length = BinaryPrimitives.ReadUInt32BigEndian(
                span.Slice(offset, sizeof(uint)));
            offset += sizeof(uint);
            if (length > (uint)(span.Length - offset))
                return false;
            offset += checked((int)length);
        }
        if (offset != span.Length)
            return false;

        var decoded = new Message[partCount];
        var created = 0;
        offset = sizeof(uint);
        try
        {
            for (var index = 0; index < decoded.Length; index++)
            {
                var length = BinaryPrimitives.ReadUInt32BigEndian(
                    span.Slice(offset, sizeof(uint)));
                offset += sizeof(uint);
                decoded[index] = Message.From(
                    span.Slice(offset, checked((int)length)));
                created++;
                offset += checked((int)length);
            }

            parts = decoded;
            return true;
        }
        finally
        {
            if (parts.Length == 0)
                for (var index = 0; index < created; index++)
                    decoded[index].Dispose();
        }
    }

    private static bool TryDecodeMultipartView(
        Message frame,
        ReadOnlySpan<byte> span,
        int payloadOffset,
        out ZLinkMultipartPayloadView? parts)
    {
        parts = null;
        if (span.Length - payloadOffset < sizeof(uint))
            return false;

        var count = BinaryPrimitives.ReadUInt32BigEndian(
            span.Slice(payloadOffset, sizeof(uint)));
        if (count == 0
            || count > int.MaxValue / 2
            || count > (span.Length - payloadOffset - sizeof(uint)) / sizeof(uint))
            return false;

        var ranges = new int[checked((int)count * 2)];
        var offset = payloadOffset + sizeof(uint);
        for (var index = 0; index < checked((int)count); index++)
        {
            if (span.Length - offset < sizeof(uint))
                return false;
            var length = BinaryPrimitives.ReadUInt32BigEndian(
                span.Slice(offset, sizeof(uint)));
            offset += sizeof(uint);
            if (length > (uint)(span.Length - offset))
                return false;
            ranges[index * 2] = offset;
            ranges[index * 2 + 1] = checked((int)length);
            offset += checked((int)length);
        }
        if (offset != span.Length)
            return false;

        parts = new ZLinkMultipartPayloadView(frame, ranges);
        return true;
    }
}

internal static class ZLinkInlineCreationIntentCodec
{
    private const string Prefix = "inline-v1:";

    internal static string Encode(ReadOnlySpan<byte> payload)
    {
        var checksum = Zlink.Framework.Runtime.Locations.ZLinkCrc32C
            .Compute(payload);
        var encoded = Convert.ToBase64String(payload)
            .TrimEnd('=')
            .Replace('+', '-')
            .Replace('/', '_');
        return string.Create(
            CultureInfo.InvariantCulture,
            $"{Prefix}{checksum:x8}:{encoded}");
    }

    internal static bool TryDecode(
        string reference,
        out byte[] payload)
    {
        payload = [];
        if (!reference.StartsWith(Prefix, StringComparison.Ordinal))
            return false;
        var checksumEnd = reference.IndexOf(':', Prefix.Length);
        if (checksumEnd != Prefix.Length + 8
            || checksumEnd + 1 >= reference.Length
            || !uint.TryParse(
                reference.AsSpan(Prefix.Length, 8),
                NumberStyles.AllowHexSpecifier,
                CultureInfo.InvariantCulture,
                out var expectedChecksum))
            return false;
        var encoded = reference.AsSpan(checksumEnd + 1);
        foreach (var value in encoded)
            if (!(value is >= 'A' and <= 'Z'
                  or >= 'a' and <= 'z'
                  or >= '0' and <= '9'
                  or '-' or '_'))
                return false;
        if (encoded.Length % 4 == 1)
            return false;
        var padded = encoded.ToString()
            .Replace('-', '+')
            .Replace('_', '/')
            .PadRight((encoded.Length + 3) / 4 * 4, '=');
        try
        {
            payload = Convert.FromBase64String(padded);
        }
        catch (FormatException)
        {
            payload = [];
            return false;
        }
        if (Zlink.Framework.Runtime.Locations.ZLinkCrc32C.Compute(payload)
            == expectedChecksum)
            return true;
        payload = [];
        return false;
    }
}
