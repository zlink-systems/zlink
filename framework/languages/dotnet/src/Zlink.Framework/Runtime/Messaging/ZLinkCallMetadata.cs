using Zlink.Framework.Runtime.Backend.DotNet;
using Systems.Zlink.Stream.Connector.Runtime;

namespace Zlink.Framework.Runtime.Messaging;

// Owns the metadata half of every mesh-plane call builder (05-route-mesh §6):
// last-write-wins accumulation and canonical-frame encoding with the 1024-byte
// cap. Call classes hold one instance so the merge/encode knowledge lives here
// rather than in ten builders.
internal sealed class ZLinkCallMetadata
{
    private Dictionary<string, string>? _values;

    public void Set(string key, string value)
    {
        ArgumentException.ThrowIfNullOrEmpty(key);
        ArgumentNullException.ThrowIfNull(value);
        (_values ??= new Dictionary<string, string>(StringComparer.Ordinal))[key] =
            value;
    }

    public void Merge(ZLinkMessageMetadata metadata)
    {
        ArgumentNullException.ThrowIfNull(metadata);
        foreach (var pair in metadata.Values)
            Set(pair.Key, pair.Value);
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
        return ZLinkMeshMetadataCodec.Encode(new ZLinkMessageMetadata(_values));
    }

    internal IReadOnlyDictionary<string, string> Snapshot() =>
        _values is null
            ? new Dictionary<string, string>(StringComparer.Ordinal)
            : new Dictionary<string, string>(_values, StringComparer.Ordinal);

    public ZlinkStreamMetadata ToStreamMetadata()
    {
        _ = Encode();
        var metadata = ZlinkStreamMetadata.Empty;
        if (_values is null) return metadata;
        foreach (var (key, value) in _values) metadata = metadata.With(key, value);
        return metadata;
    }
}
