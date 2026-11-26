using System.Runtime.CompilerServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization.Metadata;

#if ZLINK_FRAMEWORK
namespace Zlink.Framework.Runtime.Messaging;

#else
namespace Systems.Zlink.Stream.Connector.Internal;

#endif

internal static class ZLinkFrameworkJsonDocument
{
    private static readonly ConditionalWeakTable<JsonTypeInfo, Property[]> PropertyNames = new();

    private sealed record Property(byte[] Name, JsonPropertyInfo Metadata);

    private static Property[] GetProperties(JsonTypeInfo? typeInfo) =>
        typeInfo?.Kind == JsonTypeInfoKind.Object
            ? PropertyNames.GetValue(
                typeInfo,
                static type =>
                    type.Properties.Count is > 0 and <= 64
                        ? type
                            .Properties.Select(static property => new Property(
                                Encoding.UTF8.GetBytes(property.Name),
                                property
                            ))
                            .ToArray()
                        : []
            )
            : [];

    internal static void ValidateDocument(ReadOnlySpan<byte> payload, JsonTypeInfo? typeInfo = null)
    {
        if (payload.Length >= 3 && payload[0] == 0xef && payload[1] == 0xbb && payload[2] == 0xbf)
            throw new JsonException("framework-json-v1 does not allow a UTF-8 BOM.");

        try
        {
            var reader = new Utf8JsonReader(
                payload,
                new JsonReaderOptions
                {
                    CommentHandling = JsonCommentHandling.Disallow,
                    AllowTrailingCommas = false,
                }
            );
            ValidateTokens(ref reader, typeInfo, documentRoot: true);
        }
        catch (JsonException error)
        {
            throw new JsonException("framework-json-v1 payload is malformed.", error);
        }
    }

    private static void ValidateTokens(
        ref Utf8JsonReader reader,
        JsonTypeInfo? typeInfo,
        bool documentRoot = false
    )
    {
        var properties = documentRoot ? [] : GetProperties(typeInfo);
        ulong seen = 0;
        var next = 0;
        JsonPropertyInfo? property = null;
        HashSet<string>? objectProperties = null;
        while (reader.Read())
        {
            switch (reader.TokenType)
            {
                case JsonTokenType.StartObject:
                    var child = documentRoot && reader.CurrentDepth == 0 ? typeInfo : null;
                    if (property is { CustomConverter: null } && typeInfo is not null)
                        child = typeInfo.Options.GetTypeInfo(property.PropertyType);
                    ValidateTokens(ref reader, child);
                    property = null;
                    break;
                case JsonTokenType.PropertyName:
                    var index = FindProperty(ref reader, properties, next);
                    if (index >= 0)
                    {
                        var bit = 1UL << index;
                        if ((seen & bit) != 0)
                            throw new JsonException(
                                $"Duplicate JSON property '{reader.GetString()}'."
                            );
                        seen |= bit;
                        next = index + 1;
                        property = properties[index].Metadata;
                        break;
                    }

                    property = null;
                    objectProperties ??= new HashSet<string>(StringComparer.Ordinal);
                    var name =
                        reader.GetString()
                        ?? throw new JsonException("JSON property name is null.");
                    if (!objectProperties.Add(name))
                        throw new JsonException($"Duplicate JSON property '{name}'.");
                    break;
                case JsonTokenType.EndObject:
                    return;
                case JsonTokenType.Number:
                    if (!reader.TryGetDouble(out var number) || double.IsInfinity(number))
                        throw new JsonException(
                            "framework-json-v1 only accepts finite JSON numbers."
                        );
                    property = null;
                    break;
                default:
                    property = null;
                    break;
            }
        }
    }

    private static int FindProperty(ref Utf8JsonReader reader, Property[] properties, int next)
    {
        if (
            (uint)next < (uint)properties.Length
            && PropertyMatches(ref reader, properties[next].Name)
        )
            return next;
        for (var index = 0; index < properties.Length; index++)
            if (index != next && PropertyMatches(ref reader, properties[index].Name))
                return index;
        return -1;
    }

    private static bool PropertyMatches(ref Utf8JsonReader reader, byte[] name) =>
        reader.ValueIsEscaped ? reader.ValueTextEquals(name) : reader.ValueSpan.SequenceEqual(name);

#if ZLINK_FRAMEWORK
    internal static string? PropertyPathAt(ReadOnlySpan<byte> payload, long tokenOffset)
    {
        var containers = new List<(bool Array, int Index, string? Property)>();
        var reader = new Utf8JsonReader(payload);
        while (reader.Read() && reader.TokenStartIndex <= tokenOffset)
        {
            switch (reader.TokenType)
            {
                case JsonTokenType.PropertyName:
                    containers[^1] = (false, -1, reader.GetString());
                    continue;
                case JsonTokenType.EndArray:
                case JsonTokenType.EndObject:
                    containers.RemoveAt(containers.Count - 1);
                    continue;
            }

            if (containers.Count != 0 && containers[^1] is (true, var index, _))
                containers[^1] = (true, index + 1, null);
            if (reader.TokenStartIndex == tokenOffset)
            {
                var path = new StringBuilder("$");
                foreach (var container in containers)
                {
                    if (container.Array)
                        path.Append('[').Append(container.Index).Append(']');
                    else if (container.Property is { } name)
                        path.Append('.').Append(name);
                }
                return path.ToString();
            }

            if (reader.TokenType is JsonTokenType.StartArray or JsonTokenType.StartObject)
                containers.Add((reader.TokenType == JsonTokenType.StartArray, -1, null));
        }
        return null;
    }
#endif
}
