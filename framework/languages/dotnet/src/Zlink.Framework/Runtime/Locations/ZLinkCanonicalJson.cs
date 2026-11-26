using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Zlink.Framework.Runtime.Locations;

internal static class ZLinkCanonicalJson
{
    internal static JsonSerializerOptions Options { get; } =
        new() { Converters = { new CanonicalStringConverter() } };

    private sealed class CanonicalStringConverter : JsonConverter<string>
    {
        private static readonly UTF8Encoding StrictUtf8 = new(false, true);

        public override string? Read(
            ref Utf8JsonReader reader,
            Type type,
            JsonSerializerOptions options
        ) => reader.GetString();

        public override void Write(
            Utf8JsonWriter writer,
            string value,
            JsonSerializerOptions options
        )
        {
            var json = new StringBuilder(value.Length + 2).Append('"');
            foreach (var character in value)
            {
                switch (character)
                {
                    case '"':
                        json.Append("\\\"");
                        break;
                    case '\\':
                        json.Append("\\\\");
                        break;
                    case '\b':
                        json.Append("\\b");
                        break;
                    case '\t':
                        json.Append("\\t");
                        break;
                    case '\n':
                        json.Append("\\n");
                        break;
                    case '\f':
                        json.Append("\\f");
                        break;
                    case '\r':
                        json.Append("\\r");
                        break;
                    case < ' ':
                        json.Append("\\u").Append(((ushort)character).ToString("x4"));
                        break;
                    default:
                        json.Append(character);
                        break;
                }
            }
            json.Append('"');
            writer.WriteRawValue(StrictUtf8.GetBytes(json.ToString()));
        }
    }
}
