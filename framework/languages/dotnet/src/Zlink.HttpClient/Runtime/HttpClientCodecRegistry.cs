using System.Text;
using System.Text.Json;
using Zlink.Framework.Contracts.Codecs;

namespace Zlink.HttpClient.Runtime;

internal sealed class HttpClientCodecRegistry : IZLinkCodecRegistryBuilder, IZLinkCodecRegistrar
{
    private const string JsonContentType = "application/json";
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    private readonly Dictionary<string, RegisteredSerializer> _serializers =
        new(StringComparer.Ordinal);

    // Per-type resolution cache: the registry is snapshot at Build() and requests resolve the
    // same payload types repeatedly, so the linear scan runs once per type instead of per call.
    private readonly System.Collections.Concurrent.ConcurrentDictionary<
        Type, (bool Found, string ContentType, IZLinkMessageSerializer? Serializer)> _resolved = new();

    public HttpClientCodecRegistry()
    {
    }

    private HttpClientCodecRegistry(HttpClientCodecRegistry source)
    {
        foreach (var (contentType, serializer) in source._serializers) _serializers[contentType] = serializer;
    }

    public void Use(IZLinkCodecExtension extension)
    {
        ArgumentNullException.ThrowIfNull(extension);
        extension.Register(this);
    }

    public void AddSerializer(string contentType, IZLinkMessageSerializer serializer)
    {
        AddSerializer(contentType, serializer, _ => true, true);
    }

    public void AddSerializer(
        string contentType,
        IZLinkMessageSerializer serializer,
        Func<Type, bool> canSerialize)
    {
        AddSerializer(contentType, serializer, canSerialize, false);
    }

    public HttpClientCodecRegistry Snapshot()
    {
        return new HttpClientCodecRegistry(this);
    }

    public (byte[] Body, string ContentType) Encode(object? value, Type type)
    {
        if (value is null) return ([], JsonContentType);

        if (TryResolveSerializer(type, out var contentType, out var serializer)
            || TryResolveFallback(out contentType, out serializer))
        {
            var payload = serializer.Serialize(value, type);
            return (payload.ToArray(), contentType);
        }

        return (
            JsonSerializer.SerializeToUtf8Bytes(value, type, JsonOptions),
            JsonContentType);
    }

    public object? Decode(byte[] body, Type type, string? contentType)
    {
        if (type == typeof(byte[])) return body;

        if (type == typeof(ReadOnlyMemory<byte>)) return new ReadOnlyMemory<byte>(body);

        if (body.Length == 0) return type.IsValueType ? Activator.CreateInstance(type) : null;

        if (TryNormalizeResponseContentType(contentType, out var normalizedContentType)
            && _serializers.TryGetValue(normalizedContentType, out var found))
            return found.Serializer.Deserialize(ZLinkEncodedPayload.FromOwned(body), type);

        if (type == typeof(string)) return DecodeString(body);

        return JsonSerializer.Deserialize(body, type, JsonOptions);
    }

    private static string DecodeString(byte[] body)
    {
        try
        {
            return JsonSerializer.Deserialize<string>(body, JsonOptions)
                   ?? string.Empty;
        }
        catch (JsonException)
        {
            return Encoding.UTF8.GetString(body);
        }
    }

    private void AddSerializer(
        string contentType,
        IZLinkMessageSerializer serializer,
        Func<Type, bool> canSerialize,
        bool isFallbackSerializer)
    {
        ArgumentNullException.ThrowIfNull(serializer);
        ArgumentNullException.ThrowIfNull(canSerialize);
        _serializers[NormalizeRegisteredContentType(contentType)] =
            new RegisteredSerializer(serializer, canSerialize, isFallbackSerializer);
        _resolved.Clear();
    }

    private bool TryResolveSerializer(
        Type payloadType,
        out string contentType,
        out IZLinkMessageSerializer serializer)
    {
        var entry = _resolved.GetOrAdd(payloadType, ResolveUncached);
        contentType = entry.ContentType;
        serializer = entry.Serializer!;
        return entry.Found;
    }

    private (bool Found, string ContentType, IZLinkMessageSerializer? Serializer) ResolveUncached(
        Type payloadType)
    {
        var matches = _serializers
            .Where(entry => entry.Value.CanSerialize(payloadType))
            .ToArray();

        if (matches.Length == 0) return (false, string.Empty, null);

        if (matches.Length > 1)
            throw new InvalidOperationException(
                "HTTP payload serializer is ambiguous for type '" + payloadType + "': "
                + string.Join(", ", matches.Select(entry => entry.Key)));

        return (true, matches[0].Key, matches[0].Value.Serializer);
    }

    private bool TryResolveFallback(out string contentType, out IZLinkMessageSerializer serializer)
    {
        var fallbackSerializers = _serializers
            .Where(entry => entry.Value.IsFallbackSerializer)
            .ToArray();

        if (fallbackSerializers.Length == 0)
        {
            contentType = string.Empty;
            serializer = null!;
            return false;
        }

        if (fallbackSerializers.Length > 1)
            throw new InvalidOperationException(
                "HTTP payload serializer is ambiguous because more than one custom serializer is registered: "
                + string.Join(", ", fallbackSerializers.Select(entry => entry.Key)));

        contentType = fallbackSerializers[0].Key;
        serializer = fallbackSerializers[0].Value.Serializer;
        return true;
    }

    private static bool TryNormalizeResponseContentType(
        string? contentType,
        out string normalized)
    {
        if (contentType is null)
        {
            normalized = string.Empty;
            return false;
        }
        var separator = contentType.IndexOf(';');
        try
        {
            normalized = NormalizeRegisteredContentType(
                separator < 0 ? contentType : contentType[..separator]);
            return true;
        }
        catch (ArgumentException)
        {
            normalized = string.Empty;
            return false;
        }
    }

    private static string NormalizeRegisteredContentType(string contentType)
    {
        ArgumentNullException.ThrowIfNull(contentType);
        var first = 0;
        var last = contentType.Length;
        while (first < last && contentType[first] is ' ' or '\t') first++;
        while (last > first && contentType[last - 1] is ' ' or '\t') last--;

        var slash = -1;
        var normalized = new char[last - first];
        for (var source = first; source < last; source++)
        {
            var value = contentType[source];
            var target = source - first;
            if (value == '/')
            {
                if (slash >= 0 || target == 0 || source == last - 1)
                    throw InvalidContentType();
                slash = target;
                normalized[target] = value;
                continue;
            }
            if (!IsTokenCharacter(value)) throw InvalidContentType();
            normalized[target] = value is >= 'A' and <= 'Z'
                ? (char)(value + ('a' - 'A'))
                : value;
        }
        if (slash < 0) throw InvalidContentType();
        return new string(normalized);
    }

    private static bool IsTokenCharacter(char value) =>
        value is >= '0' and <= '9'
        or >= 'A' and <= 'Z'
        or >= 'a' and <= 'z'
        or '!' or '#' or '$' or '%' or '&' or '\'' or '*' or '+' or '-'
        or '.' or '^' or '_' or '`' or '|' or '~';

    private static ArgumentException InvalidContentType() =>
        new("HTTP codec content type must be a parameter-free ASCII type/subtype.", "contentType");

    private sealed record RegisteredSerializer(
        IZLinkMessageSerializer Serializer,
        Func<Type, bool> CanSerialize,
        bool IsFallbackSerializer);
}
