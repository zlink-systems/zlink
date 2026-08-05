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
    ///     codec for high-level object messaging. At most one custom serializer may be
    ///     registered; registering a second one is a configuration error.
    /// </summary>
    void AddSerializer(string contentType, IZLinkMessageSerializer serializer);

    /// <summary>
    ///     Registers a payload serializer that is used only when
    ///     <paramref name="canSerialize" /> returns <c>true</c> for the declared
    ///     payload type.
    /// </summary>
    void AddSerializer(
        string contentType,
        IZLinkMessageSerializer serializer,
        Func<Type, bool> canSerialize);

}
