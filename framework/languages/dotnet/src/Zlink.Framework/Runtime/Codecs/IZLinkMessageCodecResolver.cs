namespace Zlink.Framework.Runtime.Codecs;

internal interface IZLinkMessageCodecResolver
{
    bool TryGetSerializer(
        string contentType,
        out IZLinkMessageSerializer serializer);

    bool TryResolveStreamContentType(
        ZlinkStreamCodec codec,
        out string contentType);
}

internal interface IZLinkMessageCodecRegistry : IZLinkMessageCodecResolver
{
    (string ContentType, IZLinkMessageSerializer Serializer)? SingleCustomSerializer();

    bool TryResolveSerializer(
        Type payloadType,
        out string contentType,
        out IZLinkMessageSerializer serializer);

    IZLinkMessageCodecResolver Snapshot();
}
