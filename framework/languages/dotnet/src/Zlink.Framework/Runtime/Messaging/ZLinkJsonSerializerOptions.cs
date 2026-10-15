using System.Text.Json;
using System.Text.Json.Serialization;

namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkObjectReferenceJsonFields
{
    internal const string ActorId = "actorId";
    internal const string SpotId = "spotId";
    internal const string ObjectGeneration = "objectGeneration";
    internal const string MeshName = "meshName";
    internal const string NodeRid = "nodeRid";
}

internal sealed class ZLinkCanonicalGuidJsonConverter : JsonConverter<Guid>
{
    public override Guid Read(
        ref Utf8JsonReader reader,
        Type typeToConvert,
        JsonSerializerOptions options
    )
    {
        if (
            reader.TokenType != JsonTokenType.String
            || reader.GetString() is not { } encoded
            || !Guid.TryParseExact(encoded, "D", out var value)
            || !string.Equals(encoded, value.ToString("D"), StringComparison.Ordinal)
        )
            throw new JsonException("UUID string is not canonical lowercase D format.");
        return value;
    }

    public override void Write(Utf8JsonWriter writer, Guid value, JsonSerializerOptions options) =>
        writer.WriteStringValue(value.ToString("D"));
}
