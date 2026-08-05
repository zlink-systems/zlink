using System.Text;
using System.Text.Json;

namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkStreamPacketPayloadCodec
{
    public static object? Decode(
        ZlinkStreamHeader header,
        Message payloadMessage,
        Type messageType,
        ZLinkCodecRegistryBuilder codecs,
        IZlinkStreamCompressionCodec? compressionCodec)
    {
        try
        {
            return DecodeCore(header, payloadMessage, messageType, codecs, compressionCodec);
        }
        catch (Exception exception)
        {
            throw new ZLinkStreamPayloadDecodeException(exception);
        }
    }

    private static object? DecodeCore(
        ZlinkStreamHeader header,
        Message payloadMessage,
        Type messageType,
        ZLinkCodecRegistryBuilder codecs,
        IZlinkStreamCompressionCodec? compressionCodec)
    {
        if (messageType == typeof(Message)) return payloadMessage;

        var payload = payloadMessage.AsReadOnlyMemory();
        if ((header.Flags & ZlinkStreamHeaderFlags.PayloadCompressed) != 0)
            payload = ZLinkStreamProtocolDefaults.Decompress(compressionCodec, payload);

        if (messageType == typeof(ZlinkStreamEncodedPayload))
            return new ZlinkStreamEncodedPayload(header.Codec, payload);

        if (messageType == typeof(ReadOnlyMemory<byte>)) return payload;

        if (header.Codec == ZlinkStreamCodec.Raw)
        {
            if (messageType == typeof(string)) return Encoding.UTF8.GetString(payload.Span);

            if (messageType == typeof(byte[])) return payload.ToArray();

            throw new InvalidOperationException(
                $"Raw actor packet '{header.Name}' cannot be decoded as '{messageType}'.");
        }

        if (header.Codec == ZlinkStreamCodec.Json)
            return JsonSerializer.Deserialize(payload.Span, messageType, ZLinkJsonSerializerOptions.Default);

        if (codecs.TryResolveStreamContentType(header.Codec, out var contentType))
        {
            if (codecs.TryGetSerializer(contentType, out var serializer))
                return serializer.Deserialize(ZLinkEncodedPayload.From(payload.Span), messageType);

            throw new InvalidOperationException(
                $"Actor packet '{header.Name}' uses codec '{header.Codec}', but no matching codec extension is registered.");
        }

        throw new InvalidOperationException(
            $"Actor packet '{header.Name}' uses codec '{header.Codec}'. Register a ZlinkStreamEncodedPayload handler and decode it explicitly.");
    }

    public static ZLinkMessage DecodeMessage(
        ZlinkStreamHeader header,
        Message payloadMessage,
        ZLinkCodecRegistryBuilder codecs,
        IZlinkStreamCompressionCodec? compressionCodec)
    {
        var payload = payloadMessage.AsReadOnlyMemory();
        if ((header.Flags & ZlinkStreamHeaderFlags.PayloadCompressed) != 0)
            payload = ZLinkStreamProtocolDefaults.Decompress(compressionCodec, payload);

        return ZLinkMessage.FromStreamPayload(header.Codec, payload.ToArray(), codecs);
    }

    public static ZlinkStreamEncodedPayload Encode(
        object? message,
        Type messageType,
        ZLinkCodecRegistryBuilder codecs)
    {
        if (message is not null
            && codecs.TryResolveSerializer(messageType, out var contentType, out var serializer))
        {
            if (!codecs.TryResolveStreamCodec(contentType, out var codec))
                throw new InvalidOperationException(
                    $"Stream payload type '{messageType}' resolved to content type '{contentType}', but no stream codec maps to it.");

            var encodedPayload = serializer.Serialize(message, messageType);
            return new ZlinkStreamEncodedPayload(
                codec,
                encodedPayload.ToArray(),
                messageType);
        }

        return new ZlinkStreamEncodedPayload(
            ZlinkStreamCodec.Json,
            EncodeJson(message, messageType),
            messageType);
    }

    public static byte[] EncodeJson(object? message, Type messageType)
    {
        return ZLinkEnvelopeCodec.EncodeJsonBytes(message, messageType);
    }
}

internal sealed class ZLinkStreamPayloadDecodeException(Exception decodeException)
    : Exception("The actor packet payload could not be decoded.", decodeException)
{
    public Exception DecodeException { get; } = decodeException;
}
