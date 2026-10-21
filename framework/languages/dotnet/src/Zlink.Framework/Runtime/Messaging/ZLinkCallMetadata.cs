using Systems.Zlink.Stream.Connector.Runtime;

namespace Zlink.Framework.Runtime.Messaging;

// Owns the metadata half of every mesh-plane call builder (05-route-mesh §6):
// last-write-wins accumulation and canonical-frame encoding with the 1024-byte
// cap. Call builders allocate this owner only when metadata is supplied.
internal sealed class ZLinkCallMetadata
{
    private Dictionary<string, string>? _values;

    internal static void Set(ref ZLinkCallMetadata? metadata, string key, string value) =>
        GetOrCreate(ref metadata).Set(key, value);

    internal static void Merge(ref ZLinkCallMetadata? metadata, ZLinkMessageMetadata values)
    {
        ArgumentNullException.ThrowIfNull(values);
        foreach (var pair in values.Values)
            Set(ref metadata, pair.Key, pair.Value);
    }

    private static ZLinkCallMetadata GetOrCreate(ref ZLinkCallMetadata? metadata) =>
        metadata ??= new ZLinkCallMetadata();

    internal static ReadOnlyMemory<byte> Encode(ZLinkCallMetadata? metadata) =>
        metadata?.Encode() ?? default;

    internal static ZLinkMessageMetadata Snapshot(ZLinkCallMetadata? metadata) =>
        metadata?.Snapshot() ?? ZLinkMessageMetadata.Empty;

    internal static (
        IReadOnlyDictionary<string, string>? Header,
        ReadOnlyMemory<byte> Frame
    ) EncodeChannel(ZLinkCallMetadata? metadata, bool clientServer) =>
        clientServer ? (Snapshot(metadata).Values, default) : (null, Encode(metadata));

    internal static ZlinkStreamMetadata ToStreamMetadata(ZLinkCallMetadata? metadata) =>
        metadata?.ToStreamMetadata() ?? ZlinkStreamMetadata.Empty;

    public void Set(string key, string value)
    {
        ArgumentException.ThrowIfNullOrEmpty(key);
        ArgumentNullException.ThrowIfNull(value);
        (_values ??= new Dictionary<string, string>(StringComparer.Ordinal))[key] = value;
    }

    /// <summary>
    ///     Encodes the accumulated entries as the canonical application
    ///     metadata frame; empty when no entry was set. Throws
    ///     <see cref="ZLinkFrameworkException" /> when the encoded frame
    ///     exceeds the 1024-byte contract cap.
    /// </summary>
    public ReadOnlyMemory<byte> Encode()
    {
        if (_values is null || _values.Count == 0)
            return default;
        return ZLinkMeshMetadataCodec.Encode(Snapshot());
    }

    internal ZLinkMessageMetadata Snapshot() =>
        _values is null || _values.Count == 0
            ? ZLinkMessageMetadata.Empty
            : new ZLinkMessageMetadata(_values);

    public ZlinkStreamMetadata ToStreamMetadata()
    {
        _ = Encode();
        var metadata = ZlinkStreamMetadata.Empty;
        if (_values is null)
            return metadata;
        foreach (var (key, value) in _values)
            metadata = metadata.With(key, value);
        return metadata;
    }
}
