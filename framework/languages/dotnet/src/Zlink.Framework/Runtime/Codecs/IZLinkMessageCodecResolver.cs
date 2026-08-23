namespace Zlink.Framework.Runtime.Codecs;

internal interface IZLinkMessageCodecResolver
{
    bool TryGetSerializer(
        string contentType,
        out IZLinkMessageSerializer serializer);

    bool TryResolveStreamContentType(
        ZlinkStreamCodec codec,
        out string contentType);

    bool TryResolveSerializer(
        Type payloadType,
        out string contentType,
        out IZLinkMessageSerializer serializer);
}

internal interface IZLinkMessageCodecRegistry : IZLinkMessageCodecResolver
{
    (string ContentType, IZLinkMessageSerializer Serializer)? SingleCustomSerializer();

    IZLinkMessageCodecResolver Snapshot();
}
