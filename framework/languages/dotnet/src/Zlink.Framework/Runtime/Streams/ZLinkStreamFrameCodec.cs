using Systems.Zlink.Stream.Connector.Runtime.Protocol.Framing;

namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkStreamFrameCodec
{
    internal const int PrefixSize = ZlinkStreamFrameCodec.PrefixSize;

    public static byte[] Encode(
        ReadOnlySpan<byte> header,
        ReadOnlySpan<byte> payload)
    {
        return ZlinkStreamFrameCodec.Encode(header, payload);
    }

    public static bool TryDecode(
        ReadOnlySpan<byte> frame,
        out ReadOnlySpan<byte> header,
        out ReadOnlySpan<byte> payload)
    {
        return ZlinkStreamFrameCodec.TryDecode(frame, out header, out payload);
    }
}
