namespace Zlink.Framework.Runtime.Codecs;

internal sealed class ZLinkCodecRegistryBuilder
    : IZLinkCodecRegistryBuilder,
        IZLinkCodecRegistrar,
        IZLinkMessageCodecRegistry
{
    private readonly Dictionary<ZlinkStreamCodec, string> _contentTypesByStreamCodec = [];
    private readonly ZLinkSerializerSelectionRegistry _serializerSelections =
        CreateSerializerSelectionRegistry();

    private readonly Dictionary<string, ZlinkStreamCodec> _streamCodecsByContentType = new(
        StringComparer.Ordinal
    );

    private int _frozen;
    private ZLinkCodecRegistrySnapshot _snapshot = ZLinkCodecRegistrySnapshot.Empty;

    public IReadOnlyDictionary<string, IZLinkMessageSerializer> Serializers =>
        _serializerSelections.Serializers;

    public void Use(IZLinkCodecExtension extension)
    {
        ArgumentNullException.ThrowIfNull(extension);
        extension.Register(this);
        if (extension is IZlinkStreamCodecRegistration streamCodec)
            RegisterStreamCodec(streamCodec.ContentType, streamCodec.Codec);
    }

    public void AddSerializer(string contentType, IZLinkMessageSerializer serializer)
    {
        RegisterSerializer(contentType, serializer, _ => true);
    }

    public void AddSerializer(
        string contentType,
        IZLinkMessageSerializer serializer,
        Func<Type, bool> canSerialize
    )
    {
        RegisterSerializer(contentType, serializer, canSerialize);
    }

    internal void RegisterStreamCodec(string contentType, ZlinkStreamCodec codec)
    {
        ThrowIfFrozen();
        var normalized = ZLinkSerializerSelectionRegistry.NormalizeRegisteredContentType(
            contentType
        );
        if (_streamCodecsByContentType.TryGetValue(normalized, out var replacedCodec))
            _contentTypesByStreamCodec.Remove(replacedCodec);
        if (_contentTypesByStreamCodec.TryGetValue(codec, out var replacedContentType))
            _streamCodecsByContentType.Remove(replacedContentType);
        _streamCodecsByContentType[normalized] = codec;
        _contentTypesByStreamCodec[codec] = normalized;
        RefreshSnapshot();
    }

    private void RegisterSerializer(
        string contentType,
        IZLinkMessageSerializer serializer,
        Func<Type, bool> canSerialize
    )
    {
        ThrowIfFrozen();
        ArgumentNullException.ThrowIfNull(serializer);
        ArgumentNullException.ThrowIfNull(canSerialize);
        _serializerSelections.Add(contentType, serializer, canSerialize);
        RefreshSnapshot();
    }

    public bool TryGetSerializer(string contentType, out IZLinkMessageSerializer serializer)
    {
        return _serializerSelections.TryGetExact(contentType, out serializer);
    }

    public bool TryResolveSerializer(
        Type payloadType,
        out string contentType,
        out IZLinkMessageSerializer serializer
    )
    {
        return _serializerSelections.TryResolve(payloadType, out contentType, out serializer);
    }

    public bool TryResolveStreamCodec(string contentType, out ZlinkStreamCodec codec)
    {
        return _streamCodecsByContentType.TryGetValue(contentType, out codec);
    }

    public bool TryResolveStreamContentType(ZlinkStreamCodec codec, out string contentType)
    {
        return _contentTypesByStreamCodec.TryGetValue(codec, out contentType!);
    }

    internal ZLinkCodecRegistrySnapshot Snapshot() => Volatile.Read(ref _snapshot);

    internal void Freeze()
    {
        _serializerSelections.Freeze();
        Interlocked.Exchange(ref _frozen, 1);
    }

    IZLinkMessageCodecResolver IZLinkMessageCodecRegistry.Snapshot() => Snapshot();

    internal static ZLinkSerializerSelectionRegistry CreateSerializerSelectionRegistry() =>
        new(
            ZLinkEnvelopeCodec.DefaultContentType,
            ZLinkFrameworkJsonPayloadCodec.ReceiveSerializer
        );

    private void RefreshSnapshot()
    {
        var serializers = _serializerSelections.FrozenCopy();
        var contentTypes = new Dictionary<ZlinkStreamCodec, string>(_contentTypesByStreamCodec);
        Volatile.Write(ref _snapshot, new ZLinkCodecRegistrySnapshot(serializers, contentTypes));
    }

    private void ThrowIfFrozen()
    {
        if (Volatile.Read(ref _frozen) != 0)
            throw new InvalidOperationException(
                "The codec registry is immutable after Framework runtime startup."
            );
    }
}

internal sealed class ZLinkCodecRegistrySnapshot(
    ZLinkSerializerSelectionRegistry serializers,
    IReadOnlyDictionary<ZlinkStreamCodec, string> contentTypesByStreamCodec
) : IZLinkMessageCodecResolver
{
    internal static ZLinkCodecRegistrySnapshot Empty { get; } =
        new(
            ZLinkCodecRegistryBuilder.CreateSerializerSelectionRegistry().FrozenCopy(),
            new Dictionary<ZlinkStreamCodec, string>()
        );

    internal bool TryGetSerializer(string contentType, out IZLinkMessageSerializer serializer)
    {
        if (
            !string.IsNullOrEmpty(contentType)
            && serializers.TryGetExact(contentType, out var found)
        )
        {
            serializer = found;
            return true;
        }

        serializer = null!;
        return false;
    }

    bool IZLinkMessageCodecResolver.TryGetSerializer(
        string contentType,
        out IZLinkMessageSerializer serializer
    ) => TryGetSerializer(contentType, out serializer);

    internal bool TryResolveStreamContentType(ZlinkStreamCodec codec, out string contentType) =>
        contentTypesByStreamCodec.TryGetValue(codec, out contentType!);

    bool IZLinkMessageCodecResolver.TryResolveStreamContentType(
        ZlinkStreamCodec codec,
        out string contentType
    ) => TryResolveStreamContentType(codec, out contentType);

    bool IZLinkMessageCodecResolver.TryResolveSerializer(
        Type payloadType,
        out string contentType,
        out IZLinkMessageSerializer serializer
    ) => serializers.TryResolve(payloadType, out contentType, out serializer);
}
