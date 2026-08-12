using System.Text;
using System.Text.Json;
using Zlink.Framework.Contracts.Codecs;

namespace Zlink.HttpClient.Runtime;

internal sealed class HttpClientCodecRegistry : IZLinkCodecRegistryBuilder, IZLinkCodecRegistrar
{
    private const string JsonContentType = "application/json";
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    private readonly ZLinkSerializerSelectionRegistry _serializerSelections;

    public HttpClientCodecRegistry()
    {
        _serializerSelections = new ZLinkSerializerSelectionRegistry();
    }

    private HttpClientCodecRegistry(HttpClientCodecRegistry source)
    {
        _serializerSelections = source._serializerSelections.FrozenCopy();
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

        if (_serializerSelections.TryResolve(
                type,
                out var contentType,
                out var serializer))
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

        string? normalizedContentType = null;
        if (contentType is not null)
        {
            if (!ZLinkSerializerSelectionRegistry.TryNormalizeResponseContentType(
                    contentType,
                    out var normalized))
                throw InvalidResponseContentType(contentType);
            normalizedContentType = normalized;
            if (_serializerSelections.TryGetExact(normalized, out var found))
                return found.Deserialize(ZLinkEncodedPayload.FromOwned(body), type);
        }

        if (type == typeof(string)) return DecodeString(body);

        if (normalizedContentType is not null
            && !string.Equals(
                normalizedContentType,
                JsonContentType,
                StringComparison.Ordinal))
            throw InvalidResponseContentType(contentType!);

        if (body.Length == 0)
            return type.IsValueType ? Activator.CreateInstance(type) : null;

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
        _serializerSelections.Add(
            contentType,
            serializer,
            canSerialize,
            isFallbackSerializer);
    }

    private static ZLinkFrameworkException InvalidResponseContentType(
        string contentType) =>
        new(
            ZLinkFrameworkErrorKind.ProtocolError,
            $"HTTP response content type '{contentType}' has no registered codec.");
}
