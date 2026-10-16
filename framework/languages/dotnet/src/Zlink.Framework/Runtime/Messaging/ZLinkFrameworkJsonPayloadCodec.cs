using System.Text.Json;

namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkFrameworkJsonPayloadCodec
{
    internal static IZLinkMessageSerializer ReceiveSerializer { get; } =
        new JsonMessageSerializer();

    internal static T? Deserialize<T>(ReadOnlySpan<byte> payload)
    {
        ZLinkFrameworkJsonDocument.ValidateDocument(payload);
        return JsonSerializer.Deserialize<T>(payload, ZLinkJsonSerializerOptions.FrameworkPayload);
    }

    internal static object? Deserialize(ReadOnlySpan<byte> payload, Type type)
    {
        ZLinkFrameworkJsonDocument.ValidateDocument(payload);
        return JsonSerializer.Deserialize(
            payload,
            type,
            ZLinkJsonSerializerOptions.FrameworkPayload
        );
    }

    internal static byte[] Serialize<T>(T value) =>
        JsonSerializer.SerializeToUtf8Bytes(value, ZLinkJsonSerializerOptions.FrameworkPayload);

    internal static byte[] Serialize(object? value, Type type) =>
        JsonSerializer.SerializeToUtf8Bytes(
            value,
            type,
            ZLinkJsonSerializerOptions.FrameworkPayload
        );

    private sealed class JsonMessageSerializer
        : IZLinkMessageSerializer,
            IZLinkMessageSpanDeserializer
    {
        ZLinkEncodedPayload IZLinkMessageSerializer.Serialize(object value, Type type) =>
            ZLinkEncodedPayload.FromOwned(Serialize(value, type));

        object? IZLinkMessageSerializer.Deserialize(ZLinkEncodedPayload payload, Type type) =>
            Deserialize(payload.Bytes.Span, type);

        object? IZLinkMessageSpanDeserializer.Deserialize(ReadOnlySpan<byte> payload, Type type) =>
            Deserialize(payload, type);
    }
}
