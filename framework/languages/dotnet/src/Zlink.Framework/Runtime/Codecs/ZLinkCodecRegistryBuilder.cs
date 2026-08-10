using System.Collections.Concurrent;

namespace Zlink.Framework.Runtime.Codecs;

internal sealed class ZLinkCodecRegistryBuilder :
    IZLinkCodecRegistryBuilder,
    IZLinkCodecRegistrar,
    IZLinkMessageCodecRegistry
{
    private const int MaximumCachedSerializerTypes = 1_024;

    private readonly Dictionary<ZlinkStreamCodec, string> _contentTypesByStreamCodec =
        [];

    private readonly Dictionary<string, RegisteredSerializer> _serializers =
        new(StringComparer.Ordinal);

    private readonly List<string> _serializerRegistrationOrder = [];

    private readonly Dictionary<string, ZlinkStreamCodec> _streamCodecsByContentType =
        new(StringComparer.Ordinal);

    // Registration is completed before the runtime starts. Message paths only read this cache,
    // so first-use resolution must remain safe when several receive workers resolve one type.
    private readonly ConcurrentDictionary<Type, SerializerResolution> _serializerByType = new();
    private readonly object _serializerCacheGate = new();
    private int _frozen;
    private (string ContentType, IZLinkMessageSerializer Serializer)? _singleFallbackSerializer;
    private ZLinkCodecRegistrySnapshot _snapshot = ZLinkCodecRegistrySnapshot.Empty;

    public IReadOnlyDictionary<string, IZLinkMessageSerializer> Serializers =>
        _serializers.ToDictionary(entry => entry.Key, entry => entry.Value.Serializer,
            StringComparer.Ordinal);

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
        ThrowIfFrozen();
        var normalized = NormalizeContentType(contentType);
        if (_streamCodecsByContentType.TryGetValue(normalized, out var replacedCodec))
            _contentTypesByStreamCodec.Remove(replacedCodec);
        if (_contentTypesByStreamCodec.TryGetValue(codec, out var replacedContentType))
            _streamCodecsByContentType.Remove(replacedContentType);
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
        ThrowIfFrozen();
        ArgumentNullException.ThrowIfNull(serializer);
        ArgumentNullException.ThrowIfNull(canSerialize);
        var normalized = NormalizeContentType(contentType);
        _serializers.Remove(normalized);
        _serializerRegistrationOrder.Remove(normalized);
        _serializers[normalized] = new RegisteredSerializer(serializer, canSerialize, isFallbackSerializer);
        _serializerRegistrationOrder.Add(normalized);
        lock (_serializerCacheGate)
        {
            _serializerByType.Clear();
        }
        RefreshFallbackSerializerCache();
        RefreshSnapshot();
    }

    /// <summary>
    ///     The last registered fallback serializer with its content type, or <c>null</c>
    ///     when none is registered.
    /// </summary>
    public (string ContentType, IZLinkMessageSerializer Serializer)? SingleCustomSerializer()
    {
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
            serializer = cached.Serializer!;
            return cached.Found;
        }

        SerializerResolution resolved;
        lock (_serializerCacheGate)
        {
            if (!_serializerByType.TryGetValue(payloadType, out resolved))
            {
                resolved = ResolveSerializer(payloadType);
                if (_serializerByType.Count < MaximumCachedSerializerTypes)
                    _serializerByType[payloadType] = resolved;
            }
        }

        contentType = resolved.ContentType;
        serializer = resolved.Serializer!;
        return resolved.Found;
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

    internal void Freeze()
    {
        Interlocked.Exchange(ref _frozen, 1);
    }

    IZLinkMessageCodecResolver IZLinkMessageCodecRegistry.Snapshot() => Snapshot();

    private sealed record RegisteredSerializer(
        IZLinkMessageSerializer Serializer,
        Func<Type, bool> CanSerialize,
        bool IsFallbackSerializer);

    private readonly record struct SerializerResolution(
        bool Found,
        string ContentType,
        IZLinkMessageSerializer? Serializer);

    private SerializerResolution ResolveSerializer(Type payloadType)
    {
        for (var index = _serializerRegistrationOrder.Count - 1;
             index >= 0;
             index--)
        {
            var registeredContentType = _serializerRegistrationOrder[index];
            var entry = _serializers[registeredContentType];
            if (entry.CanSerialize(payloadType))
            {
                return new SerializerResolution(
                    true,
                    registeredContentType,
                    entry.Serializer);
            }
        }

        return new SerializerResolution(false, string.Empty, null);
    }

    private void RefreshFallbackSerializerCache()
    {
        _singleFallbackSerializer = null;
        foreach (var contentType in _serializerRegistrationOrder)
        {
            var entry = _serializers[contentType];
            if (!entry.IsFallbackSerializer)
                continue;
            _singleFallbackSerializer = (contentType, entry.Serializer);
        }
    }

    private void RefreshSnapshot()
    {
        var serializers = _serializers.ToDictionary(
            entry => entry.Key,
            entry => entry.Value.Serializer,
            StringComparer.Ordinal);
        var contentTypes = new Dictionary<ZlinkStreamCodec, string>(_contentTypesByStreamCodec);
        Volatile.Write(ref _snapshot, new ZLinkCodecRegistrySnapshot(serializers, contentTypes));
    }

    private static string NormalizeContentType(string contentType)
    {
        ArgumentNullException.ThrowIfNull(contentType);
        var first = 0;
        var last = contentType.Length;
        while (first < last && IsOuterWhitespace(contentType[first])) first++;
        while (last > first && IsOuterWhitespace(contentType[last - 1])) last--;

        var slash = -1;
        var normalized = new char[last - first];
        for (var source = first; source < last; source++)
        {
            var value = contentType[source];
            var target = source - first;
            if (value == '/')
            {
                if (slash >= 0 || target == 0 || source == last - 1)
                    throw InvalidContentType(nameof(contentType));
                slash = target;
                normalized[target] = value;
                continue;
            }
            if (!IsTokenCharacter(value))
                throw InvalidContentType(nameof(contentType));
            normalized[target] = value is >= 'A' and <= 'Z'
                ? (char)(value + ('a' - 'A'))
                : value;
        }
        if (slash < 0)
            throw InvalidContentType(nameof(contentType));
        return new string(normalized);
    }

    private static bool IsOuterWhitespace(char value) =>
        value is ' ' or '\t';

    private static bool IsTokenCharacter(char value) =>
        value is >= '0' and <= '9'
        or >= 'A' and <= 'Z'
        or >= 'a' and <= 'z'
        or '!' or '#' or '$' or '%' or '&' or '\'' or '*' or '+' or '-'
        or '.' or '^' or '_' or '`' or '|' or '~';

    private static ArgumentException InvalidContentType(string parameterName) =>
        new(
            "Codec content type must be a parameter-free ASCII type/subtype.",
            parameterName);

    private void ThrowIfFrozen()
    {
        if (Volatile.Read(ref _frozen) != 0)
            throw new InvalidOperationException(
                "The codec registry is immutable after Framework runtime startup.");
    }
}

internal sealed class ZLinkCodecRegistrySnapshot(
    IReadOnlyDictionary<string, IZLinkMessageSerializer> serializers,
    IReadOnlyDictionary<ZlinkStreamCodec, string> contentTypesByStreamCodec) :
    IZLinkMessageCodecResolver
{
    internal static ZLinkCodecRegistrySnapshot Empty { get; } = new(
        new Dictionary<string, IZLinkMessageSerializer>(StringComparer.Ordinal),
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
