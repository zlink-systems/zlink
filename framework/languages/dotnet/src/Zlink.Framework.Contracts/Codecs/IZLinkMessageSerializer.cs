namespace Zlink.Framework.Contracts.Codecs;

/// <summary>
///     Custom payload serializer for high-level object messaging. Convert a business
///     object to and from framework-owned encoded bytes. Register an implementation
///     through an <see cref="IZLinkCodecExtension" /> under a content
///     type (for example <c>"application/avro"</c>) and the framework uses it to
///     encode and decode channel payloads.
/// </summary>
public interface IZLinkMessageSerializer
{
    /// <summary>Serializes <paramref name="value" /> (declared as <paramref name="type" />) to encoded bytes.</summary>
    ZLinkEncodedPayload Serialize(object value, Type type);

    /// <summary>Deserializes <paramref name="payload" /> back into an instance of <paramref name="type" />.</summary>
    object? Deserialize(ZLinkEncodedPayload payload, Type type);
}
