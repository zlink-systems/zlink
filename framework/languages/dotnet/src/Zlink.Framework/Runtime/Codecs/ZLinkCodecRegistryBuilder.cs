using System.Collections.Concurrent;

namespace Zlink.Framework.Runtime.Codecs;

internal sealed class ZLinkCodecRegistryBuilder :
    IZLinkCodecRegistryBuilder,
    IZLinkCodecRegistrar,
    IZLinkMessageCodecRegistry
{
    private readonly Dictionary<ZlinkStreamCodec, string> _contentTypesByStreamCodec =
        [];

    private readonly Dictionary<string, RegisteredSerializer> _serializers =
        new(StringComparer.OrdinalIgnoreCase);

    private readonly Dictionary<string, ZlinkStreamCodec> _streamCodecsByContentType =
        new(StringComparer.OrdinalIgnoreCase);

    // Registration is completed before the runtime starts. Message paths only read this cache,
    // so first-use resolution must remain safe when several receive workers resolve one type.
    private readonly ConcurrentDictionary<Type, (string ContentType, IZLinkMessageSerializer Serializer)>
        _serializerByType = new();
    private (string ContentType, IZLinkMessageSerializer Serializer)? _singleFallbackSerializer;
    private bool _fallbackSerializerAmbiguous;
    private ZLinkCodecRegistrySnapshot _snapshot = ZLinkCodecRegistrySnapshot.Empty;

    public IReadOnlyDictionary<string, IZLinkMessageSerializer> Serializers =>
        _serializers.ToDictionary(entry => entry.Key, entry => entry.Value.Serializer,
            StringComparer.OrdinalIgnoreCase);

    public void Use(IZLinkCodecExtension extension)
    {
        ArgumentNullException.ThrowIfNull(extension);
        extension.Register(this);
        if (extension is IZlinkStreamCodecRegistration streamCodec)
            RegisterStreamCodec(streamCodec.ContentType, streamCodec.Codec);
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

    internal void RegisterStreamCodec(
        string contentType,
        ZlinkStreamCodec codec)
    {
        if (string.IsNullOrWhiteSpace(contentType))
            throw new ArgumentException("Stream codec content type must not be blank.", nameof(contentType));

        var normalized = contentType.Trim();
        _streamCodecsByContentType[normalized] = codec;
        _contentTypesByStreamCodec[codec] = normalized;
        RefreshSnapshot();
    }

    private void AddSerializer(
        string contentType,
        IZLinkMessageSerializer serializer,
        Func<Type, bool> canSerialize,
        bool isFallbackSerializer)
    {
        ArgumentNullException.ThrowIfNull(serializer);
        ArgumentNullException.ThrowIfNull(canSerialize);
        if (string.IsNullOrWhiteSpace(contentType))
            throw new ArgumentException("Custom serializer content type must not be blank.", nameof(contentType));

        var normalized = contentType.Trim();
        _serializers[normalized] = new RegisteredSerializer(serializer, canSerialize, isFallbackSerializer);
        _serializerByType.Clear();
        RefreshFallbackSerializerCache();
        RefreshSnapshot();
    }

    /// <summary>
    ///     The single registered custom serializer with its content type, or <c>null</c>
    ///     when none is registered. Throws when more than one is registered because the
    ///     outgoing payload serializer would then be ambiguous.
    /// </summary>
    public (string ContentType, IZLinkMessageSerializer Serializer)? SingleCustomSerializer()
    {
        if (_fallbackSerializerAmbiguous)
            throw new InvalidOperationException(
                "Payload serializer is ambiguous because more than one custom serializer is registered: "
                + string.Join(", ", _serializers
                    .Where(entry => entry.Value.IsFallbackSerializer)
                    .Select(entry => entry.Key)));

        return _singleFallbackSerializer;
    }

    public bool TryGetSerializer(string contentType, out IZLinkMessageSerializer serializer)
    {
        if (!string.IsNullOrEmpty(contentType) && _serializers.TryGetValue(contentType, out var found))
        {
            serializer = found.Serializer;
            return true;
        }

        serializer = null!;
        return false;
    }

    public bool TryResolveSerializer(Type payloadType, out string contentType, out IZLinkMessageSerializer serializer)
    {
        if (_serializerByType.TryGetValue(payloadType, out var cached))
        {
            contentType = cached.ContentType;
            serializer = cached.Serializer;
            return true;
        }

        string? resolvedContentType = null;
        IZLinkMessageSerializer? resolvedSerializer = null;
        var matches = 0;

        foreach (var entry in _serializers)
        {
            if (!entry.Value.CanSerialize(payloadType))
                continue;

            matches++;
            resolvedContentType = entry.Key;
            resolvedSerializer = entry.Value.Serializer;
            if (matches > 1)
                break;
        }

        if (matches == 0)
        {
            contentType = string.Empty;
            serializer = null!;
            return false;
        }

        if (matches > 1)
            throw new InvalidOperationException(
                "Payload serializer is ambiguous for type '" + payloadType + "': "
                + string.Join(", ", _serializers
                    .Where(entry => entry.Value.CanSerialize(payloadType))
                    .Select(entry => entry.Key)));

        contentType = resolvedContentType!;
        serializer = resolvedSerializer!;
        _serializerByType[payloadType] = (contentType, serializer);
        return true;
    }

    public bool TryResolveStreamCodec(string contentType, out ZlinkStreamCodec codec)
    {
        return _streamCodecsByContentType.TryGetValue(contentType, out codec);
    }

    public bool TryResolveStreamContentType(
        ZlinkStreamCodec codec,
        out string contentType)
    {
        return _contentTypesByStreamCodec.TryGetValue(codec, out contentType!);
    }

    internal ZLinkCodecRegistrySnapshot Snapshot() => Volatile.Read(ref _snapshot);

    IZLinkMessageCodecResolver IZLinkMessageCodecRegistry.Snapshot() => Snapshot();

    private sealed record RegisteredSerializer(
        IZLinkMessageSerializer Serializer,
        Func<Type, bool> CanSerialize,
        bool IsFallbackSerializer);

    private void RefreshFallbackSerializerCache()
    {
        _singleFallbackSerializer = null;
        _fallbackSerializerAmbiguous = false;

        foreach (var entry in _serializers)
        {
            if (!entry.Value.IsFallbackSerializer)
                continue;

            if (_singleFallbackSerializer is not null)
            {
                _fallbackSerializerAmbiguous = true;
                _singleFallbackSerializer = null;
                return;
            }

            _singleFallbackSerializer = (entry.Key, entry.Value.Serializer);
        }
    }

    private void RefreshSnapshot()
    {
        var serializers = _serializers.ToDictionary(
            entry => entry.Key,
            entry => entry.Value.Serializer,
            StringComparer.OrdinalIgnoreCase);
        var contentTypes = new Dictionary<ZlinkStreamCodec, string>(_contentTypesByStreamCodec);
        Volatile.Write(ref _snapshot, new ZLinkCodecRegistrySnapshot(serializers, contentTypes));
    }
}

internal sealed class ZLinkCodecRegistrySnapshot(
    IReadOnlyDictionary<string, IZLinkMessageSerializer> serializers,
    IReadOnlyDictionary<ZlinkStreamCodec, string> contentTypesByStreamCodec) :
    IZLinkMessageCodecResolver
{
    internal static ZLinkCodecRegistrySnapshot Empty { get; } = new(
        new Dictionary<string, IZLinkMessageSerializer>(StringComparer.OrdinalIgnoreCase),
        new Dictionary<ZlinkStreamCodec, string>());

    internal bool TryGetSerializer(string contentType, out IZLinkMessageSerializer serializer)
    {
        if (!string.IsNullOrEmpty(contentType)
            && serializers.TryGetValue(contentType, out var found))
        {
            serializer = found;
            return true;
        }

        serializer = null!;
        return false;
    }

    bool IZLinkMessageCodecResolver.TryGetSerializer(
        string contentType,
        out IZLinkMessageSerializer serializer) =>
        TryGetSerializer(contentType, out serializer);

    internal bool TryResolveStreamContentType(
        ZlinkStreamCodec codec,
        out string contentType) => contentTypesByStreamCodec.TryGetValue(codec, out contentType!);

    bool IZLinkMessageCodecResolver.TryResolveStreamContentType(
        ZlinkStreamCodec codec,
        out string contentType) =>
        TryResolveStreamContentType(codec, out contentType);
}
