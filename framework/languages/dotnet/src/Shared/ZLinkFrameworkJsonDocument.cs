using System.Linq.Expressions;
using System.Reflection;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;

#if ZLINK_FRAMEWORK
namespace Zlink.Framework.Runtime.Messaging;

#else
namespace Systems.Zlink.Stream.Connector.Internal;

#endif

internal static class ZLinkFrameworkJsonDocument
{
    private delegate T ReadValue<T>(ref Utf8JsonReader reader, JsonSerializerOptions options);

    // The options' existing type-info cache owns the reader and its DTO metadata.
    // No per-message constructor argument array or required-property BitArray is needed.
    internal sealed class Resolver : IJsonTypeInfoResolver
    {
        private readonly IJsonTypeInfoResolver _metadata;

        internal Resolver(IJsonTypeInfoResolver? metadata = null) =>
            _metadata = metadata ?? new DefaultJsonTypeInfoResolver();

        public JsonTypeInfo? GetTypeInfo(Type type, JsonSerializerOptions options)
        {
            var info = _metadata.GetTypeInfo(type, options);
            if (info is null)
                return null;
            ZLinkJsonSerializerOptions.ApplyApplicationDtoMetadata(info);
            return (JsonTypeInfo)
                typeof(ZLinkFrameworkJsonDocument)
                    .GetMethod(nameof(Wrap), BindingFlags.Static | BindingFlags.NonPublic)!
                    .MakeGenericMethod(type)
                    .Invoke(null, [info])!;
        }
    }

    private static JsonTypeInfo Wrap<T>(JsonTypeInfo info) =>
        JsonMetadataServices.CreateValueInfo<T>(info.Options, new Reader<T>((JsonTypeInfo<T>)info));

    private interface IReader
    {
        object? ReadObject(ref Utf8JsonReader reader, JsonSerializerOptions options);
    }

    private sealed class Reader<T> : JsonConverter<T>, IReader
    {
        private readonly JsonTypeInfo<T> _metadata;
        private readonly ReadValue<T> _read;

        internal Reader(JsonTypeInfo<T> metadata)
        {
            _metadata = metadata;
            _read = BuildReader(metadata);
        }

        public override bool HandleNull => true;

        public override T Read(
            ref Utf8JsonReader reader,
            Type type,
            JsonSerializerOptions options
        ) => ReadTyped<T>(ref reader, options, true, this);

        internal T ReadCore(ref Utf8JsonReader reader, JsonSerializerOptions options) =>
            _read(ref reader, options);

        object? IReader.ReadObject(ref Utf8JsonReader reader, JsonSerializerOptions options) =>
            Read(ref reader, typeof(T), options);

        public override void Write(Utf8JsonWriter writer, T value, JsonSerializerOptions options) =>
            JsonSerializer.Serialize(writer, value, _metadata);
    }

    internal static T? Deserialize<T>(ReadOnlySpan<byte> payload, JsonSerializerOptions options)
    {
        options.MakeReadOnly(populateMissingResolver: true);
        var reader = Start(payload, options.MaxDepth);
        var result = ReadTyped<T>(ref reader, options, true);
        Finish(ref reader);
        return result;
    }

    internal static object? Deserialize(
        ReadOnlySpan<byte> payload,
        Type type,
        JsonSerializerOptions options
    )
    {
        options.MakeReadOnly(populateMissingResolver: true);
        var reader = Start(payload, options.MaxDepth);
        var result = ((IReader)options.GetConverter(type)).ReadObject(ref reader, options);
        Finish(ref reader);
        return result;
    }

    private static Utf8JsonReader Start(ReadOnlySpan<byte> payload, int maxDepth = 0)
    {
        // Utf8JsonReader rejects the BOM, comments and trailing commas itself.
        var reader = new Utf8JsonReader(payload, new JsonReaderOptions { MaxDepth = maxDepth });
        Next(ref reader);
        return reader;
    }

    private static void Finish(ref Utf8JsonReader reader)
    {
        if (reader.Read())
            throw new JsonException("Trailing JSON data.");
    }

    private static void Next(ref Utf8JsonReader reader)
    {
        if (!reader.Read())
            throw new JsonException("Incomplete JSON payload.");
    }

    private static T ReadTyped<T>(
        ref Utf8JsonReader reader,
        JsonSerializerOptions options,
        bool allowsNull,
        JsonConverter<T>? declaredConverter = null
    )
    {
        var converter = declaredConverter ?? (JsonConverter<T>)options.GetConverter(typeof(T));
        if (reader.TokenType == JsonTokenType.Null && typeof(T) != typeof(JsonElement))
        {
            if (
                !allowsNull
                || (typeof(T).IsValueType && Nullable.GetUnderlyingType(typeof(T)) is null)
            )
                throw new JsonException("The declared JSON value does not allow null.");
            if (
                declaredConverter is null
                || !declaredConverter.HandleNull
                || declaredConverter is Reader<T>
            )
                return default!;
        }
        Finite(ref reader);
        if (converter is Reader<T> typed)
            return typed.ReadCore(ref reader, options);
        return ReadLeaf(ref reader, options, converter);
    }

    private static T ReadLeaf<T>(
        ref Utf8JsonReader reader,
        JsonSerializerOptions options,
        JsonConverter<T> converter
    )
    {
        try
        {
            return converter.Read(ref reader, typeof(T), options)!;
        }
        catch (InvalidOperationException error)
        {
            throw new JsonException("JSON token does not match the declared type.", error);
        }
        catch (FormatException error)
        {
            throw new JsonException("JSON value does not match the declared type.", error);
        }
        catch (OverflowException error)
        {
            throw new JsonException("JSON value is outside the declared range.", error);
        }
    }

    private static void Finite(ref Utf8JsonReader reader)
    {
        if (
            reader.TokenType == JsonTokenType.Number
            && (!reader.TryGetDouble(out var value) || double.IsInfinity(value))
        )
            throw new JsonException("framework-json-v1 only accepts finite JSON numbers.");
    }

    private static ReadValue<T> BuildReader<T>(JsonTypeInfo<T> metadata)
    {
        if (metadata.Kind == JsonTypeInfoKind.Object)
            return BuildObjectReader(metadata);
        var type = typeof(T);
        if (Nullable.GetUnderlyingType(type) is { } underlying)
            return (ReadValue<T>)
                typeof(ZLinkFrameworkJsonDocument)
                    .GetMethod(
                        nameof(NullableReader),
                        BindingFlags.Static | BindingFlags.NonPublic
                    )!
                    .MakeGenericMethod(underlying)
                    .Invoke(null, null)!;
        if (
            metadata.Kind == JsonTypeInfoKind.Dictionary
            || metadata.Kind == JsonTypeInfoKind.Enumerable
        )
        {
            var dictionary = type.GetInterfaces()
                .Append(type)
                .FirstOrDefault(i =>
                    i.IsGenericType
                    && (
                        i.GetGenericTypeDefinition() == typeof(IDictionary<,>)
                        || i.GetGenericTypeDefinition() == typeof(IReadOnlyDictionary<,>)
                    )
                );
            if (dictionary is not null && dictionary.GetGenericArguments()[0] == typeof(string))
                return (ReadValue<T>)
                    typeof(ZLinkFrameworkJsonDocument)
                        .GetMethod(
                            nameof(DictionaryReader),
                            BindingFlags.Static | BindingFlags.NonPublic
                        )!
                        .MakeGenericMethod(type, dictionary.GetGenericArguments()[1])
                        .Invoke(null, null)!;
            var enumerable = type.GetInterfaces()
                .Append(type)
                .First(i =>
                    i.IsGenericType && i.GetGenericTypeDefinition() == typeof(IEnumerable<>)
                );
            return (ReadValue<T>)
                typeof(ZLinkFrameworkJsonDocument)
                    .GetMethod(
                        nameof(CollectionReader),
                        BindingFlags.Static | BindingFlags.NonPublic
                    )!
                    .MakeGenericMethod(type, enumerable.GetGenericArguments()[0])
                    .Invoke(null, null)!;
        }
        if (type == typeof(JsonElement) || type == typeof(object))
            return (ref Utf8JsonReader reader, JsonSerializerOptions _) =>
                (T)(object)ReadElement(ref reader);
        var converter = (JsonConverter<T>)metadata.Converter;
        return (ref Utf8JsonReader reader, JsonSerializerOptions options) =>
            ReadLeaf(ref reader, options, converter);
    }

    private static ReadValue<T?> NullableReader<T>()
        where T : struct =>
        (ref Utf8JsonReader reader, JsonSerializerOptions options) =>
            ReadTyped<T>(ref reader, options, false);

    private static ReadValue<TCollection> CollectionReader<TCollection, TElement>() =>
        (ref Utf8JsonReader reader, JsonSerializerOptions options) =>
        {
            Require(ref reader, JsonTokenType.StartArray);
            var values = new List<TElement>();
            while (true)
            {
                Next(ref reader);
                if (reader.TokenType == JsonTokenType.EndArray)
                    break;
                values.Add(ReadTyped<TElement>(ref reader, options, true));
            }
            if (typeof(TCollection).IsArray)
                return (TCollection)(object)values.ToArray();
            if (values is TCollection collection)
                return collection;
            return (TCollection)Activator.CreateInstance(typeof(TCollection), [values])!;
        };

    private static ReadValue<TDictionary> DictionaryReader<TDictionary, TValue>() =>
        (ref Utf8JsonReader reader, JsonSerializerOptions options) =>
        {
            Require(ref reader, JsonTokenType.StartObject);
            var values = new Dictionary<string, TValue>(StringComparer.Ordinal);
            while (true)
            {
                Next(ref reader);
                if (reader.TokenType == JsonTokenType.EndObject)
                    break;
                Require(ref reader, JsonTokenType.PropertyName);
                var name = reader.GetString()!;
                Unique(values.ContainsKey(name));
                Next(ref reader);
                values.Add(name, ReadTyped<TValue>(ref reader, options, true));
            }
            if (values is TDictionary dictionary)
                return dictionary;
            return (TDictionary)Activator.CreateInstance(typeof(TDictionary), [values])!;
        };

    private static void Require(ref Utf8JsonReader reader, JsonTokenType token)
    {
        if (reader.TokenType != token)
            throw new JsonException($"Expected {token}.");
    }

    private static void Unique(bool duplicate)
    {
        if (duplicate)
            throw new JsonException("Duplicate JSON property.");
    }

    private static bool Named(ref Utf8JsonReader reader, byte[] name) =>
        reader.ValueTextEquals(name);

    private static void Unknown(
        ref Utf8JsonReader reader,
        ref HashSet<string>? names,
        bool disallow
    )
    {
        var name = reader.GetString()!;
        names ??= new HashSet<string>(StringComparer.Ordinal);
        Unique(!names.Add(name));
        if (disallow)
            throw new JsonException($"Unknown JSON property '{name}'.");
        Next(ref reader);
        Skip(ref reader);
    }

    internal static void ValidateDocument(ReadOnlySpan<byte> payload)
    {
        var reader = Start(payload);
        Visit(ref reader, null);
        Finish(ref reader);
    }

    private static void Skip(ref Utf8JsonReader reader) => Visit(ref reader, null);

    // DOM materialization consumes the source through the same validated reader.
    // Preserve raw string and number tokens when constructing the owned DOM.
    private static JsonElement ReadElement(ref Utf8JsonReader reader)
    {
        using var stream = new MemoryStream();
        Visit(ref reader, stream);
        using var document = JsonDocument.Parse(stream.ToArray());
        return document.RootElement.Clone();
    }

    private static void Visit(ref Utf8JsonReader reader, System.IO.Stream? output)
    {
        Finite(ref reader);
        switch (reader.TokenType)
        {
            case JsonTokenType.StartObject:
                output?.WriteByte((byte)'{');
                HashSet<string>? names = null;
                while (true)
                {
                    Next(ref reader);
                    if (reader.TokenType == JsonTokenType.EndObject)
                        break;
                    Require(ref reader, JsonTokenType.PropertyName);
                    var name = reader.GetString()!;
                    names ??= new HashSet<string>(StringComparer.Ordinal);
                    Unique(!names.Add(name));
                    if (names.Count > 1)
                        output?.WriteByte((byte)',');
                    StringToken(ref reader, output);
                    output?.WriteByte((byte)':');
                    Next(ref reader);
                    Visit(ref reader, output);
                }
                output?.WriteByte((byte)'}');
                break;
            case JsonTokenType.StartArray:
                output?.WriteByte((byte)'[');
                var count = 0;
                while (true)
                {
                    Next(ref reader);
                    if (reader.TokenType == JsonTokenType.EndArray)
                        break;
                    if (count++ > 0)
                        output?.WriteByte((byte)',');
                    Visit(ref reader, output);
                }
                output?.WriteByte((byte)']');
                break;
            case JsonTokenType.String:
                StringToken(ref reader, output);
                break;
            case JsonTokenType.Number:
            case JsonTokenType.True:
            case JsonTokenType.False:
            case JsonTokenType.Null:
                output?.Write(reader.ValueSpan);
                break;
            default:
                throw new JsonException("Expected a JSON value.");
        }
    }

    private static void StringToken(ref Utf8JsonReader reader, System.IO.Stream? output)
    {
        if (output is null)
            return;
        output.WriteByte((byte)'"');
        output.Write(reader.ValueSpan);
        output.WriteByte((byte)'"');
    }

    private static ReadValue<T> BuildObjectReader<T>(JsonTypeInfo<T> metadata)
    {
        var reader = Expression.Parameter(typeof(Utf8JsonReader).MakeByRefType(), "reader");
        var options = Expression.Parameter(typeof(JsonSerializerOptions), "options");
        var result = Expression.Variable(typeof(T), "result");
        var unknown = Expression.Variable(typeof(HashSet<string>), "unknown");
        var properties = metadata.Properties.ToArray();
        var values = properties.Select(p => Expression.Variable(p.PropertyType, p.Name)).ToArray();
        var seen = properties
            .Select(p => Expression.Variable(typeof(bool), "seen_" + p.Name))
            .ToArray();
        var constructor = ZLinkJsonSerializerOptions.ApplicationConstructor(typeof(T));
        var parameters = constructor?.GetParameters() ?? [];
        int MemberIndex(ParameterInfo parameter) =>
            Array.FindIndex(
                properties,
                p =>
                    string.Equals(
                        (p.AttributeProvider as MemberInfo)?.Name ?? p.Name,
                        parameter.Name,
                        StringComparison.OrdinalIgnoreCase
                    )
            );
        var arguments = parameters.Select(p => MemberIndex(p)).ToArray();
        if (arguments.Any(i => i < 0))
            throw new InvalidOperationException(
                $"Unbound JSON constructor parameter in {typeof(T)}."
            );
        var body = new List<Expression>
        {
            Call(nameof(Require), reader, Expression.Constant(JsonTokenType.StartObject)),
        };
        for (var n = 0; n < parameters.Length; n++)
            if (parameters[n].HasDefaultValue)
                body.Add(
                    Expression.Assign(
                        values[arguments[n]],
                        parameters[n].DefaultValue is { } defaultValue
                            ? Expression.Convert(
                                Expression.Constant(defaultValue),
                                parameters[n].ParameterType
                            )
                            : Expression.Default(parameters[n].ParameterType)
                    )
                );
        var done = Expression.Label("done");
        Expression dispatch = Call(
            nameof(Unknown),
            reader,
            unknown,
            Expression.Constant(
                (metadata.UnmappedMemberHandling ?? metadata.Options.UnmappedMemberHandling)
                    == JsonUnmappedMemberHandling.Disallow
            )
        );
        for (var i = properties.Length - 1; i >= 0; i--)
        {
            var p = properties[i];
            var parameterIndex = Array.IndexOf(arguments, i);
            var read = typeof(ZLinkFrameworkJsonDocument)
                .GetMethod(nameof(ReadTyped), BindingFlags.Static | BindingFlags.NonPublic)!
                .MakeGenericMethod(p.PropertyType);
            var assign = Expression.Block(
                Call(nameof(Unique), seen[i]),
                Expression.Assign(seen[i], Expression.Constant(true)),
                Call(nameof(Next), reader),
                Expression.Assign(
                    values[i],
                    Expression.Call(
                        read,
                        reader,
                        options,
                        Expression.Constant(
                            ZLinkJsonSerializerOptions.AllowsNull(
                                p,
                                parameterIndex >= 0 ? parameters[parameterIndex] : null
                            )
                        ),
                        Expression.Constant(
                            p.CustomConverter is JsonConverterFactory factory
                                ? factory.CreateConverter(p.PropertyType, metadata.Options)
                                : p.CustomConverter,
                            typeof(JsonConverter<>).MakeGenericType(p.PropertyType)
                        )
                    )
                ),
                Expression.Empty()
            );
            dispatch = Expression.IfThenElse(
                Call(nameof(Named), reader, Expression.Constant(Encoding.UTF8.GetBytes(p.Name))),
                assign,
                dispatch
            );
        }
        body.Add(
            Expression.Loop(
                Expression.Block(
                    Call(nameof(Next), reader),
                    Expression.IfThen(
                        Expression.Equal(
                            Expression.Property(reader, nameof(Utf8JsonReader.TokenType)),
                            Expression.Constant(JsonTokenType.EndObject)
                        ),
                        Expression.Break(done)
                    ),
                    Call(nameof(Require), reader, Expression.Constant(JsonTokenType.PropertyName)),
                    dispatch
                ),
                done
            )
        );
        for (var i = 0; i < properties.Length; i++)
            if (properties[i].IsRequired)
                body.Add(
                    Expression.IfThen(
                        Expression.Not(seen[i]),
                        Throw($"Missing required JSON property '{properties[i].Name}'.")
                    )
                );
        Expression create =
            constructor is not null ? Expression.New(constructor, arguments.Select(i => values[i]))
            : metadata.CreateObject is not null
                ? Expression.Invoke(Expression.Constant(metadata.CreateObject))
            : typeof(T).IsValueType ? Expression.Default(typeof(T))
            : Expression.Throw(Expression.New(typeof(NotSupportedException)), typeof(T));
        body.Add(Expression.Assign(result, create));
        if (metadata.OnDeserializing is not null)
            body.Add(
                Expression.Invoke(
                    Expression.Constant(metadata.OnDeserializing),
                    Expression.Convert(result, typeof(object))
                )
            );
        for (var i = 0; i < properties.Length; i++)
            if (!arguments.Contains(i) && properties[i].Set is { } setter)
                body.Add(
                    Expression.IfThen(
                        seen[i],
                        Expression.Invoke(
                            Expression.Constant(setter),
                            Expression.Convert(result, typeof(object)),
                            Expression.Convert(values[i], typeof(object))
                        )
                    )
                );
        if (metadata.OnDeserialized is not null)
            body.Add(
                Expression.Invoke(
                    Expression.Constant(metadata.OnDeserialized),
                    Expression.Convert(result, typeof(object))
                )
            );
        body.Add(result);
        return Expression
            .Lambda<ReadValue<T>>(
                Expression.Block(new[] { result, unknown }.Concat(values).Concat(seen), body),
                reader,
                options
            )
            .Compile();
    }

    private static MethodCallExpression Call(string name, params Expression[] arguments) =>
        Expression.Call(
            typeof(ZLinkFrameworkJsonDocument).GetMethod(
                name,
                BindingFlags.Static | BindingFlags.NonPublic
            )!,
            arguments
        );

    private static UnaryExpression Throw(string message) =>
        Expression.Throw(
            Expression.New(
                typeof(JsonException).GetConstructor([typeof(string)])!,
                Expression.Constant(message)
            )
        );
}
