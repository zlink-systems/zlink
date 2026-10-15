using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;

#if ZLINK_FRAMEWORK
namespace Zlink.Framework.Runtime.Messaging;

#else
namespace Systems.Zlink.Stream.Connector.Internal;

#endif

internal static class ZLinkJsonSerializerOptions
{
    // Runtime control and Store DTOs keep their own wire contracts. Application
    // payloads must use FrameworkPayload through ZLinkFrameworkJsonPayloadCodec.
#if ZLINK_FRAMEWORK
    public static JsonSerializerOptions Default { get; } = CreateDefault();
#endif

    public static JsonSerializerOptions FrameworkPayload { get; } = CreateFrameworkPayload();

#if ZLINK_FRAMEWORK
    private static JsonSerializerOptions CreateDefault()
    {
        var options = new JsonSerializerOptions(JsonSerializerDefaults.Web);
        options.Converters.Add(new RoutingIdJsonConverter());
        return options;
    }
#endif

    private static JsonSerializerOptions CreateFrameworkPayload()
    {
        var options = new JsonSerializerOptions(JsonSerializerDefaults.Web)
        {
            PropertyNameCaseInsensitive = false,
            NumberHandling = JsonNumberHandling.Strict,
        };
        options.Converters.Add(new FrameworkSigned64JsonConverter());
        options.Converters.Add(new FrameworkUnsigned64JsonConverter());
        options.Converters.Add(new FrameworkSingleJsonConverter());
        options.Converters.Add(new FrameworkEnumJsonConverterFactory());
        options.Converters.Add(new UnsupportedImplicitTypeJsonConverterFactory());
#if ZLINK_FRAMEWORK
        options.Converters.Add(new RoutingIdJsonConverter());
#endif
        var resolver = new DefaultJsonTypeInfoResolver();
        resolver.Modifiers.Add(RequireDeclaredNullability);
        options.TypeInfoResolver = resolver;
        return options;
    }

    private static void RequireDeclaredNullability(JsonTypeInfo typeInfo)
    {
        if (typeInfo.Kind != JsonTypeInfoKind.Object)
            return;
#if NET6_0_OR_GREATER
        var nullability = new System.Reflection.NullabilityInfoContext();
#endif
        foreach (var property in typeInfo.Properties)
        {
            if (property.PropertyType.IsValueType || property.Set is null)
                continue;
#if NET6_0_OR_GREATER
            var state = property.AttributeProvider switch
            {
                System.Reflection.PropertyInfo reflectedProperty => nullability
                    .Create(reflectedProperty)
                    .WriteState,
                System.Reflection.FieldInfo reflectedField => nullability
                    .Create(reflectedField)
                    .WriteState,
                _ => System.Reflection.NullabilityState.Unknown,
            };
            if (state != System.Reflection.NullabilityState.NotNull)
                continue;

#else
            if (!IsDeclaredNotNull(property.AttributeProvider))
                continue;
#endif

            var assign = property.Set;
            property.Set = (instance, value) =>
            {
                if (value is null)
                    throw new JsonException($"Property '{property.Name}' does not allow null.");
                assign(instance, value);
            };
        }
    }

#if NETSTANDARD2_1
    private const byte NotNullFlag = 1;
    private const string NullableAttributeName =
        "System.Runtime.CompilerServices.NullableAttribute";
    private const string NullableContextAttributeName =
        "System.Runtime.CompilerServices.NullableContextAttribute";

    private static bool IsDeclaredNotNull(System.Reflection.ICustomAttributeProvider? provider)
    {
        if (provider is not System.Reflection.MemberInfo member)
            return false;
        var declared = NullableFlag(member, NullableAttributeName);
        if (declared.HasValue)
            return declared.Value == NotNullFlag;
        for (var type = member.DeclaringType; type is not null; type = type.DeclaringType)
        {
            var context = NullableFlag(type, NullableContextAttributeName);
            if (context.HasValue)
                return context.Value == NotNullFlag;
        }
        return false;
    }

    private static byte? NullableFlag(System.Reflection.MemberInfo member, string attributeName)
    {
        foreach (var attribute in member.GetCustomAttributesData())
        {
            if (
                attribute.AttributeType.FullName != attributeName
                || attribute.ConstructorArguments.Count != 1
            )
                continue;
            var argument = attribute.ConstructorArguments[0];
            if (argument.Value is byte flag)
                return flag;
            if (
                argument.Value is IList<System.Reflection.CustomAttributeTypedArgument> flags
                && flags.Count > 0
            )
                return (byte)flags[0].Value!;
        }
        return null;
    }
#endif

    // Store opaque-record envelope DTOs (Runtime/Locations) reuse these two
    // converters directly for the canonical decimal-string generation
    // fields pinned by 21-location-runtime.md#2.4, so they can't stay
    // private to this class.
    internal sealed class FrameworkSigned64JsonConverter : JsonConverter<long>
    {
        public override long Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options
        )
        {
            if (reader.TokenType != JsonTokenType.String)
                throw new JsonException("Signed 64-bit integers must be decimal strings.");
            var encoded = reader.GetString();
            if (
                encoded is null
                || !long.TryParse(
                    encoded,
                    System.Globalization.NumberStyles.AllowLeadingSign,
                    System.Globalization.CultureInfo.InvariantCulture,
                    out var value
                )
                || !string.Equals(
                    encoded,
                    value.ToString(System.Globalization.CultureInfo.InvariantCulture),
                    StringComparison.Ordinal
                )
            )
                throw new JsonException("Signed 64-bit integer string is not canonical.");
            return value;
        }

        public override void Write(
            Utf8JsonWriter writer,
            long value,
            JsonSerializerOptions options
        ) =>
            writer.WriteStringValue(
                value.ToString(System.Globalization.CultureInfo.InvariantCulture)
            );
    }

    internal sealed class FrameworkUnsigned64JsonConverter : JsonConverter<ulong>
    {
        public override ulong Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options
        )
        {
            if (reader.TokenType != JsonTokenType.String)
                throw new JsonException("Unsigned 64-bit integers must be decimal strings.");
            var encoded = reader.GetString();
            if (
                encoded is null
                || !ulong.TryParse(
                    encoded,
                    System.Globalization.NumberStyles.None,
                    System.Globalization.CultureInfo.InvariantCulture,
                    out var value
                )
                || !string.Equals(
                    encoded,
                    value.ToString(System.Globalization.CultureInfo.InvariantCulture),
                    StringComparison.Ordinal
                )
            )
                throw new JsonException("Unsigned 64-bit integer string is not canonical.");
            return value;
        }

        public override void Write(
            Utf8JsonWriter writer,
            ulong value,
            JsonSerializerOptions options
        ) =>
            writer.WriteStringValue(
                value.ToString(System.Globalization.CultureInfo.InvariantCulture)
            );
    }

    private sealed class FrameworkSingleJsonConverter : JsonConverter<float>
    {
        public override float Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options
        )
        {
            if (
                reader.TokenType != JsonTokenType.Number
                || !reader.TryGetSingle(out var value)
                || float.IsInfinity(value)
            )
                throw new JsonException(
                    "framework-json-v1 requires a finite single-precision JSON number."
                );
            return value;
        }

        public override void Write(
            Utf8JsonWriter writer,
            float value,
            JsonSerializerOptions options
        ) => writer.WriteNumberValue(value);
    }

    private sealed class FrameworkEnumJsonConverterFactory : JsonConverterFactory
    {
        public override bool CanConvert(Type typeToConvert) => typeToConvert.IsEnum;

        public override JsonConverter CreateConverter(
            Type typeToConvert,
            JsonSerializerOptions options
        ) =>
            (JsonConverter)(
                Activator.CreateInstance(
                    typeof(FrameworkEnumJsonConverter<>).MakeGenericType(typeToConvert),
                    nonPublic: true
                )
                ?? throw new InvalidOperationException(
                    $"Cannot create a framework JSON converter for '{typeToConvert}'."
                )
            );
    }

    private sealed class FrameworkEnumJsonConverter<TEnum> : JsonConverter<TEnum>
        where TEnum : struct, Enum
    {
        private static readonly IReadOnlyDictionary<string, TEnum> Values = Enum.GetNames(
                typeof(TEnum)
            )
            .ToDictionary(
                static name => name,
                static name => Enum.Parse<TEnum>(name),
                StringComparer.Ordinal
            );

        public override TEnum Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options
        )
        {
            if (
                reader.TokenType != JsonTokenType.String
                || reader.GetString() is not { } name
                || !Values.TryGetValue(name, out var value)
            )
                throw new JsonException(
                    $"Enum '{typeof(TEnum).Name}' requires an exact declared name."
                );
            return value;
        }

        public override void Write(
            Utf8JsonWriter writer,
            TEnum value,
            JsonSerializerOptions options
        )
        {
            var name =
                Enum.GetName(typeof(TEnum), value)
                ?? throw new JsonException(
                    $"Enum '{typeof(TEnum).Name}' value '{value}' has no declared name."
                );
            writer.WriteStringValue(name);
        }
    }

    private sealed class UnsupportedImplicitTypeJsonConverterFactory : JsonConverterFactory
    {
        private const string DateOnlyTypeName = "System.DateOnly";
        private const string TimeOnlyTypeName = "System.TimeOnly";
        private static readonly HashSet<Type> Unsupported =
        [
            typeof(DateTime),
            typeof(DateTimeOffset),
            typeof(decimal),
            typeof(Guid),
        ];

        public override bool CanConvert(Type typeToConvert)
        {
            var type = Nullable.GetUnderlyingType(typeToConvert) ?? typeToConvert;
            return Unsupported.Contains(type)
                || type.FullName is DateOnlyTypeName or TimeOnlyTypeName;
        }

        public override JsonConverter CreateConverter(
            Type typeToConvert,
            JsonSerializerOptions options
        ) =>
            (JsonConverter)(
                Activator.CreateInstance(
                    typeof(UnsupportedImplicitTypeJsonConverter<>).MakeGenericType(typeToConvert),
                    nonPublic: true
                )
                ?? throw new InvalidOperationException(
                    $"Cannot create a rejecting JSON converter for '{typeToConvert}'."
                )
            );
    }

    private sealed class UnsupportedImplicitTypeJsonConverter<T> : JsonConverter<T>
    {
        public override T? Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options
        ) =>
            throw new JsonException(
                $"Type '{typeof(T).Name}' requires an explicit framework JSON string or DTO contract."
            );

        public override void Write(Utf8JsonWriter writer, T value, JsonSerializerOptions options) =>
            throw new JsonException(
                $"Type '{typeof(T).Name}' requires an explicit framework JSON string or DTO contract."
            );
    }

#if ZLINK_FRAMEWORK
    private sealed class RoutingIdJsonConverter : JsonConverter<RoutingId>
    {
        public override RoutingId Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options
        )
        {
            var hex = reader.GetString();
            return string.IsNullOrEmpty(hex) ? default : RoutingId.FromHex(hex);
        }

        public override void Write(
            Utf8JsonWriter writer,
            RoutingId value,
            JsonSerializerOptions options
        )
        {
            writer.WriteStringValue(value.IsEmpty ? string.Empty : value.ToHex());
        }
    }
#endif
}
