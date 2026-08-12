namespace Zlink.Framework.Contracts.Codecs;

public interface IZLinkCodecRegistryBuilder
{
    void Use(IZLinkCodecExtension extension);
}

public interface IZLinkCodecRegistrar
{
    /// <summary>
    ///     Registers a custom payload serializer under a content type (for example
    ///     <c>"application/avro"</c>). The registered serializer becomes the payload
    ///     codec for high-level object messaging. If more than one fallback serializer
    ///     is registered, the last registration is selected.
    /// </summary>
    void AddSerializer(string contentType, IZLinkMessageSerializer serializer);

    /// <summary>
    ///     Registers a payload serializer that is used only when
    ///     <paramref name="canSerialize" /> returns <c>true</c> for the declared
    ///     payload type. If more than one selector accepts the declared type, the last
    ///     registration is selected.
    /// </summary>
    void AddSerializer(
        string contentType,
        IZLinkMessageSerializer serializer,
        Func<Type, bool> canSerialize);

}
