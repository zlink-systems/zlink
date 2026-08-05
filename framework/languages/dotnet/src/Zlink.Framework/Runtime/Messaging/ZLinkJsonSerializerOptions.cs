using System.Text.Json;
using System.Text.Json.Serialization;

namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkJsonSerializerOptions
{
    public static JsonSerializerOptions Default { get; } = CreateDefault();

    private static JsonSerializerOptions CreateDefault()
    {
        var options = new JsonSerializerOptions(JsonSerializerDefaults.Web);
        options.Converters.Add(new RoutingIdJsonConverter());
        return options;
    }

    private sealed class RoutingIdJsonConverter : JsonConverter<RoutingId>
    {
        public override RoutingId Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
        {
            var hex = reader.GetString();
            return string.IsNullOrEmpty(hex) ? default : RoutingId.FromHex(hex);
        }

        public override void Write(Utf8JsonWriter writer, RoutingId value, JsonSerializerOptions options)
        {
            writer.WriteStringValue(value.IsEmpty ? string.Empty : value.ToHex());
        }
    }
}
