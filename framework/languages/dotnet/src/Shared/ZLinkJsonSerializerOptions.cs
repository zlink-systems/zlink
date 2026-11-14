using System.Buffers;
using System.Buffers.Text;
using System.Reflection;
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
        options.Converters.Add(new FrameworkByteArrayJsonConverter());
        options.Converters.Add(new FrameworkEnumJsonConverterFactory());
        options.Converters.Add(new UnsupportedImplicitTypeJsonConverterFactory());
#if ZLINK_FRAMEWORK
        options.Converters.Add(new RoutingIdJsonConverter());
#endif
        var resolver = new DefaultJsonTypeInfoResolver();
        resolver.Modifiers.Add(ApplyApplicationDtoMetadata);
        options.TypeInfoResolver = resolver;
        return options;
    }

    private static void ApplyApplicationDtoMetadata(JsonTypeInfo typeInfo)
    {
        if (typeInfo.Kind != JsonTypeInfoKind.Object)
            return;
        var constructor = ApplyConstructorRequiredness(typeInfo);
        RequireDeclaredNullability(typeInfo, constructor);
    }

    private static ConstructorInfo? ApplyConstructorRequiredness(JsonTypeInfo typeInfo)
    {
        if (Nullable.GetUnderlyingType(typeInfo.Type) is not null)
            return null;

        var constructors = typeInfo.Type.GetConstructors(
            BindingFlags.Instance | BindingFlags.Public
        );
        var constructor = constructors.FirstOrDefault(candidate =>
            candidate.IsDefined(typeof(JsonConstructorAttribute), inherit: false)
        );
        if (constructor is null)
        {
            if (constructors.Any(candidate => candidate.GetParameters().Length == 0))
                return null;
            var publicParameterized = constructors.Where(candidate =>
                candidate.GetParameters().Length > 0
            );
            constructor =
                publicParameterized.Take(2).Count() == 1 ? publicParameterized.First() : null;
        }
        if (constructor is null)
            return null;

        foreach (var parameter in constructor.GetParameters())
        {
            if (parameter.Name is null)
                continue;
            var property = typeInfo.Properties.FirstOrDefault(candidate =>
                string.Equals(
                    candidate.AttributeProvider switch
                    {
                        PropertyInfo reflected => reflected.Name,
                        FieldInfo reflected => reflected.Name,
                        _ => candidate.Name,
                    },
                    parameter.Name,
                    StringComparison.OrdinalIgnoreCase
                )
            );
            if (property is null)
                continue;
            property.Set ??= static (_, _) => { };
            property.IsRequired = !parameter.HasDefaultValue;
        }
        return constructor;
    }

    private static void RequireDeclaredNullability(
        JsonTypeInfo typeInfo,
        ConstructorInfo? constructor
    )
    {
#if NET6_0_OR_GREATER
        var nullability = new System.Reflection.NullabilityInfoContext();
#endif
        var parameters = constructor?.GetParameters();
        foreach (var property in typeInfo.Properties)
        {
            if (property.PropertyType.IsValueType || property.Set is null)
                continue;
            var memberName = property.AttributeProvider is MemberInfo member
                ? member.Name
                : property.Name;
            var parameter = parameters?.FirstOrDefault(candidate =>
                string.Equals(candidate.Name, memberName, StringComparison.OrdinalIgnoreCase)
            );
#if NET6_0_OR_GREATER
            var state = parameter is not null
                ? nullability.Create(parameter).WriteState
                : property.AttributeProvider switch
                {
                    PropertyInfo reflectedProperty => nullability
                        .Create(reflectedProperty)
                        .WriteState,
                    FieldInfo reflectedField => nullability.Create(reflectedField).WriteState,
                    _ => System.Reflection.NullabilityState.Unknown,
                };
            if (state != System.Reflection.NullabilityState.NotNull)
                continue;
#else
            if (
                !IsDeclaredNotNull(
                    (System.Reflection.ICustomAttributeProvider?)parameter
                        ?? property.AttributeProvider
                )
            )
                continue;
#endif
            var converter =
                property.CustomConverter ?? typeInfo.Options.GetConverter(property.PropertyType);
            if (converter is JsonConverterFactory factory)
                converter = factory.CreateConverter(property.PropertyType, typeInfo.Options)!;
            // STJ also uses the matching property's converter for constructor arguments.
            property.CustomConverter = (JsonConverter)
                Activator.CreateInstance(
                    typeof(NonNullJsonConverter<>).MakeGenericType(property.PropertyType),
                    converter,
                    property.Name
                )!;
        }
    }

    private sealed class NonNullJsonConverter<T>(JsonConverter<T> converter, string propertyName)
        : JsonConverter<T>
        where T : class
    {
        public override bool HandleNull => true;

        public override T? Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options
        )
        {
            if (reader.TokenType == JsonTokenType.Null)
                throw new JsonException($"Property '{propertyName}' does not allow null.");
            return converter.Read(ref reader, typeToConvert, options);
        }

        public override void Write(Utf8JsonWriter writer, T value, JsonSerializerOptions options) =>
            converter.Write(writer, value, options);
    }

#if NETSTANDARD2_1
    private const byte NotNullFlag = 1;
    private const string NullableAttributeName =
        "System.Runtime.CompilerServices.NullableAttribute";
    private const string NullableContextAttributeName =
        "System.Runtime.CompilerServices.NullableContextAttribute";

    private static bool IsDeclaredNotNull(System.Reflection.ICustomAttributeProvider? provider)
    {
        var member = provider as MemberInfo ?? (provider as ParameterInfo)?.Member;
        if (member is null)
            return false;
        var declared = NullableFlag(provider!, NullableAttributeName);
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

    private static byte? NullableFlag(
        System.Reflection.ICustomAttributeProvider provider,
        string attributeName
    )
    {
        var attributes = provider switch
        {
            ParameterInfo parameter => parameter.GetCustomAttributesData(),
            MemberInfo member => member.GetCustomAttributesData(),
            _ => Array.Empty<CustomAttributeData>(),
        };
        foreach (var attribute in attributes)
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

    private static bool IsCanonicalDecimalInteger(string? encoded)
    {
        if (string.IsNullOrEmpty(encoded))
            return false;
        if (encoded == "0")
            return true;
        var firstDigit = encoded[0] == '-' ? 1 : 0;
        if (firstDigit == encoded.Length || encoded[firstDigit] < '1' || encoded[firstDigit] > '9')
            return false;
        for (var index = firstDigit + 1; index < encoded.Length; index++)
        {
            if (encoded[index] < '0' || encoded[index] > '9')
                return false;
        }
        return true;
    }

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
                !IsCanonicalDecimalInteger(encoded)
                || !long.TryParse(
                    encoded,
                    System.Globalization.NumberStyles.AllowLeadingSign,
                    System.Globalization.CultureInfo.InvariantCulture,
                    out var value
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
                !IsCanonicalDecimalInteger(encoded)
                || !ulong.TryParse(
                    encoded,
                    System.Globalization.NumberStyles.None,
                    System.Globalization.CultureInfo.InvariantCulture,
                    out var value
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

    private sealed class FrameworkByteArrayJsonConverter : JsonConverter<byte[]>
    {
        public override byte[] Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options
        )
        {
            if (reader.TokenType != JsonTokenType.String)
                throw new JsonException("Byte sequences must be Base64 strings.");

            // One decode path: obtain the unescaped token bytes, then decode once.
            byte[]? copied = null;
            ReadOnlySpan<byte> encoded;
            if (!reader.HasValueSequence && !reader.ValueIsEscaped)
            {
                encoded = reader.ValueSpan;
            }
            else
            {
                copied = ArrayPool<byte>.Shared.Rent(
                    reader.HasValueSequence
                        ? checked((int)reader.ValueSequence.Length)
                        : reader.ValueSpan.Length
                );
                encoded = copied.AsSpan(0, reader.CopyString(copied));
            }

            try
            {
                return Decode(encoded);
            }
            finally
            {
                if (copied is not null)
                    ArrayPool<byte>.Shared.Return(copied);
            }
        }

        private static byte[] Decode(ReadOnlySpan<byte> encoded)
        {
            if ((encoded.Length & 3) != 0)
                throw new JsonException("Byte sequence requires padded RFC 4648 Base64.");
            var padding =
                encoded.Length == 0 || encoded[^1] != '=' ? 0
                : encoded[^2] == '=' ? 2
                : 1;
            if (padding != 0)
            {
                var ch = encoded[encoded.Length - padding - 1];
                // Only letters and digits can have zero padding bits. The BCL
                // below still owns validation of the complete Base64 alphabet.
                var last =
                    ch >= (byte)'a' ? ch - 'a' + 26
                    : ch >= (byte)'A' ? ch - 'A'
                    : ch >= (byte)'0' ? ch - '0' + 52
                    : -1;
                if ((padding == 2 && (last & 15) != 0) || (padding == 1 && (last & 3) != 0))
                    throw new JsonException("Byte sequence has nonzero Base64 padding bits.");
            }
            // With a padded input length, skipped whitespace necessarily makes
            // the BCL write fewer bytes than this exact canonical length.
            var length = encoded.Length / 4 * 3 - padding;
            var decoded = length == 0 ? Array.Empty<byte>() : new byte[length];
            if (
                Base64.DecodeFromUtf8(encoded, decoded, out var consumed, out var written)
                    != OperationStatus.Done
                || consumed != encoded.Length
                || written != length
            )
                throw new JsonException("Byte sequence is not valid Base64.");
            return decoded;
        }

        public override void Write(
            Utf8JsonWriter writer,
            byte[] value,
            JsonSerializerOptions options
        ) => writer.WriteBase64StringValue(value);
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
