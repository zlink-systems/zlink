using System.Text.Json;

namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkFrameworkJsonPayloadCodec
{
    internal static T? Deserialize<T>(ReadOnlySpan<byte> payload)
    {
        ValidateDocument(payload);
        return JsonSerializer.Deserialize<T>(
            payload,
            ZLinkJsonSerializerOptions.FrameworkPayload);
    }

    internal static object? Deserialize(ReadOnlySpan<byte> payload, Type type)
    {
        ValidateDocument(payload);
        return JsonSerializer.Deserialize(
            payload,
            type,
            ZLinkJsonSerializerOptions.FrameworkPayload);
    }

    internal static byte[] Serialize<T>(T value) =>
        JsonSerializer.SerializeToUtf8Bytes(
            value,
            ZLinkJsonSerializerOptions.FrameworkPayload);

    internal static byte[] Serialize(object? value, Type type) =>
        JsonSerializer.SerializeToUtf8Bytes(
            value,
            type,
            ZLinkJsonSerializerOptions.FrameworkPayload);

    private static void ValidateDocument(ReadOnlySpan<byte> payload)
    {
        if (payload.Length >= 3
            && payload[0] == 0xef
            && payload[1] == 0xbb
            && payload[2] == 0xbf)
            throw new JsonException("framework-json-v1 does not allow a UTF-8 BOM.");

        try
        {
            var reader = new Utf8JsonReader(payload, new JsonReaderOptions
            {
                CommentHandling = JsonCommentHandling.Disallow,
                AllowTrailingCommas = false
            });
            var objectProperties = new Stack<HashSet<string>>();
            while (reader.Read())
            {
                switch (reader.TokenType)
                {
                    case JsonTokenType.StartObject:
                        objectProperties.Push(new HashSet<string>(StringComparer.Ordinal));
                        break;
                    case JsonTokenType.PropertyName:
                        var name = reader.GetString()
                                   ?? throw new JsonException("JSON property name is null.");
                        if (objectProperties.Count == 0
                            || !objectProperties.Peek().Add(name))
                            throw new JsonException($"Duplicate JSON property '{name}'.");
                        break;
                    case JsonTokenType.EndObject:
                        objectProperties.Pop();
                        break;
                }
            }
        }
        catch (JsonException error)
        {
            throw new JsonException("framework-json-v1 payload is malformed.", error);
        }
    }
}
