using MessagePack;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Codecs;

namespace Zlink.Framework.Codecs.MessagePack;

public sealed class ZLinkMessagePackCodec :
    IZLinkCodecExtension,
    IZlinkStreamPayloadCodec,
    IZlinkStreamCodecRegistration
{
    private ZLinkMessagePackCodec()
    {
    }

    public static ZLinkMessagePackCodec Default { get; } = new();

    public void Register(IZLinkCodecRegistrar codecs)
    {
        ArgumentNullException.ThrowIfNull(codecs);
        codecs.AddSerializer(
            "application/x-msgpack",
            MessagePackSerializerAdapter.Instance,
            type => type.GetCustomAttributes(typeof(MessagePackObjectAttribute), true).Length > 0);
    }

    string IZlinkStreamCodecRegistration.ContentType => "application/x-msgpack";

    ZlinkStreamCodec IZlinkStreamCodecRegistration.Codec => ZlinkStreamCodec.MessagePack;

    public ZlinkStreamEncodedPayload Encode<TPayload>(TPayload payload)
    {
        return new ZlinkStreamEncodedPayload(
            ZlinkStreamCodec.MessagePack,
            MessagePackSerializer.Serialize(payload, MessagePackSerializerOptions.Standard),
            typeof(TPayload));
    }

    public TPayload Decode<TPayload>(ZlinkStreamEncodedPayload payload)
    {
        if (payload.Codec != ZlinkStreamCodec.MessagePack)
            throw new InvalidOperationException($"Stream payload codec is {payload.Codec}, not MessagePack.");

        return MessagePackSerializer.Deserialize<TPayload>(
            payload.Payload,
            MessagePackSerializerOptions.Standard);
    }

    private sealed class MessagePackSerializerAdapter : IZLinkMessageSerializer
    {
        public static MessagePackSerializerAdapter Instance { get; } = new();

        public ZLinkEncodedPayload Serialize(object value, Type type)
        {
            return ZLinkEncodedPayload.From(
                MessagePackSerializer.Serialize(type, value, MessagePackSerializerOptions.Standard));
        }

        public object? Deserialize(ZLinkEncodedPayload payload, Type type)
        {
            return MessagePackSerializer.Deserialize(type, payload.Bytes, MessagePackSerializerOptions.Standard);
        }
    }
}
