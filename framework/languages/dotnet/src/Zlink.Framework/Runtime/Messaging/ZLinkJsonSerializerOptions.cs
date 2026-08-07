using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;

namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkJsonSerializerOptions
{
    // Runtime control and Store DTOs keep their own wire contracts. Application
    // payloads must use FrameworkPayload through ZLinkFrameworkJsonPayloadCodec.
    public static JsonSerializerOptions Default { get; } = CreateDefault();

    public static JsonSerializerOptions FrameworkPayload { get; } = CreateFrameworkPayload();

    private static JsonSerializerOptions CreateDefault()
    {
        var options = new JsonSerializerOptions(JsonSerializerDefaults.Web);
        options.Converters.Add(new RoutingIdJsonConverter());
        return options;
    }

    private static JsonSerializerOptions CreateFrameworkPayload()
    {
        var options = new JsonSerializerOptions(JsonSerializerDefaults.Web)
        {
            PropertyNameCaseInsensitive = false,
            NumberHandling = JsonNumberHandling.Strict
        };
        options.Converters.Add(new FrameworkSigned64JsonConverter());
        options.Converters.Add(new FrameworkUnsigned64JsonConverter());
        options.Converters.Add(new FrameworkEnumJsonConverterFactory());
        options.Converters.Add(new UnsupportedImplicitTypeJsonConverterFactory());
        options.Converters.Add(new RoutingIdJsonConverter());
        var resolver = new DefaultJsonTypeInfoResolver();
        resolver.Modifiers.Add(RequireDeclaredNullability);
        options.TypeInfoResolver = resolver;
        return options;
    }

    private static void RequireDeclaredNullability(JsonTypeInfo typeInfo)
    {
        if (typeInfo.Kind != JsonTypeInfoKind.Object) return;
        var nullability = new System.Reflection.NullabilityInfoContext();
        foreach (var property in typeInfo.Properties)
        {
            if (property.PropertyType.IsValueType || property.Set is null) continue;
            var state = property.AttributeProvider switch
            {
                System.Reflection.PropertyInfo reflectedProperty =>
                    nullability.Create(reflectedProperty).WriteState,
                System.Reflection.FieldInfo reflectedField =>
                    nullability.Create(reflectedField).WriteState,
                _ => System.Reflection.NullabilityState.Unknown
            };
            if (state != System.Reflection.NullabilityState.NotNull) continue;

            var assign = property.Set;
            property.Set = (instance, value) =>
            {
                if (value is null)
                    throw new JsonException(
                        $"Property '{property.Name}' does not allow null.");
                assign(instance, value);
            };
        }
    }

    private sealed class FrameworkSigned64JsonConverter : JsonConverter<long>
    {
        public override long Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options)
        {
            if (reader.TokenType != JsonTokenType.String)
                throw new JsonException("Signed 64-bit integers must be decimal strings.");
            var encoded = reader.GetString();
            if (encoded is null
                || !long.TryParse(
                    encoded,
                    System.Globalization.NumberStyles.AllowLeadingSign,
                    System.Globalization.CultureInfo.InvariantCulture,
                    out var value)
                || !string.Equals(
                    encoded,
                    value.ToString(System.Globalization.CultureInfo.InvariantCulture),
                    StringComparison.Ordinal))
                throw new JsonException("Signed 64-bit integer string is not canonical.");
            return value;
        }

        public override void Write(
            Utf8JsonWriter writer,
            long value,
            JsonSerializerOptions options) =>
            writer.WriteStringValue(value.ToString(System.Globalization.CultureInfo.InvariantCulture));
    }

    private sealed class FrameworkUnsigned64JsonConverter : JsonConverter<ulong>
    {
        public override ulong Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options)
        {
            if (reader.TokenType != JsonTokenType.String)
                throw new JsonException("Unsigned 64-bit integers must be decimal strings.");
            var encoded = reader.GetString();
            if (encoded is null
                || !ulong.TryParse(
                    encoded,
                    System.Globalization.NumberStyles.None,
                    System.Globalization.CultureInfo.InvariantCulture,
                    out var value)
                || !string.Equals(
                    encoded,
                    value.ToString(System.Globalization.CultureInfo.InvariantCulture),
                    StringComparison.Ordinal))
                throw new JsonException("Unsigned 64-bit integer string is not canonical.");
            return value;
        }

        public override void Write(
            Utf8JsonWriter writer,
            ulong value,
            JsonSerializerOptions options) =>
            writer.WriteStringValue(value.ToString(System.Globalization.CultureInfo.InvariantCulture));
    }

    private sealed class FrameworkEnumJsonConverterFactory : JsonConverterFactory
    {
        public override bool CanConvert(Type typeToConvert) => typeToConvert.IsEnum;

        public override JsonConverter CreateConverter(
            Type typeToConvert,
            JsonSerializerOptions options) =>
            (JsonConverter)(Activator.CreateInstance(
                typeof(FrameworkEnumJsonConverter<>).MakeGenericType(typeToConvert),
                nonPublic: true)
                ?? throw new InvalidOperationException(
                    $"Cannot create a framework JSON converter for '{typeToConvert}'."));
    }

    private sealed class FrameworkEnumJsonConverter<TEnum> : JsonConverter<TEnum>
        where TEnum : struct, Enum
    {
        private static readonly IReadOnlyDictionary<string, TEnum> Values =
            Enum.GetNames<TEnum>().ToDictionary(
                static name => name,
                static name => Enum.Parse<TEnum>(name),
                StringComparer.Ordinal);

        public override TEnum Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options)
        {
            if (reader.TokenType != JsonTokenType.String
                || reader.GetString() is not { } name
                || !Values.TryGetValue(name, out var value))
                throw new JsonException($"Enum '{typeof(TEnum).Name}' requires an exact declared name.");
            return value;
        }

        public override void Write(
            Utf8JsonWriter writer,
            TEnum value,
            JsonSerializerOptions options)
        {
            var name = Enum.GetName(value)
                       ?? throw new JsonException(
                           $"Enum '{typeof(TEnum).Name}' value '{value}' has no declared name.");
            writer.WriteStringValue(name);
        }
    }

    private sealed class UnsupportedImplicitTypeJsonConverterFactory : JsonConverterFactory
    {
        private static readonly HashSet<Type> Unsupported =
        [
            typeof(DateTime),
            typeof(DateTimeOffset),
            typeof(DateOnly),
            typeof(TimeOnly),
            typeof(decimal),
            typeof(Guid)
        ];

        public override bool CanConvert(Type typeToConvert) =>
            Unsupported.Contains(Nullable.GetUnderlyingType(typeToConvert) ?? typeToConvert);

        public override JsonConverter CreateConverter(
            Type typeToConvert,
            JsonSerializerOptions options) =>
            (JsonConverter)(Activator.CreateInstance(
                typeof(UnsupportedImplicitTypeJsonConverter<>).MakeGenericType(typeToConvert),
                nonPublic: true)
                ?? throw new InvalidOperationException(
                    $"Cannot create a rejecting JSON converter for '{typeToConvert}'."));
    }

    private sealed class UnsupportedImplicitTypeJsonConverter<T> : JsonConverter<T>
    {
        public override T? Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options) =>
            throw new JsonException(
                $"Type '{typeof(T).Name}' requires an explicit framework JSON string or DTO contract.");

        public override void Write(
            Utf8JsonWriter writer,
            T value,
            JsonSerializerOptions options) =>
            throw new JsonException(
                $"Type '{typeof(T).Name}' requires an explicit framework JSON string or DTO contract.");
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

internal sealed class ZLinkCanonicalGuidJsonConverter : JsonConverter<Guid>
{
    public override Guid Read(
        ref Utf8JsonReader reader,
        Type typeToConvert,
        JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.String
            || reader.GetString() is not { } encoded
            || !Guid.TryParseExact(encoded, "D", out var value)
            || !string.Equals(encoded, value.ToString("D"), StringComparison.Ordinal))
            throw new JsonException("UUID string is not canonical lowercase D format.");
        return value;
    }

    public override void Write(
        Utf8JsonWriter writer,
        Guid value,
        JsonSerializerOptions options) =>
        writer.WriteStringValue(value.ToString("D"));
}
