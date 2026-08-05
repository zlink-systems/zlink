namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkStreamActorPacketKind
{
    public static bool TryMap(
        ZlinkStreamMessageKind streamKind,
        out ZLinkMessageKind messageKind)
    {
        switch (streamKind)
        {
            case ZlinkStreamMessageKind.Send:
                messageKind = ZLinkMessageKind.Command;
                return true;
            case ZlinkStreamMessageKind.Request:
                messageKind = ZLinkMessageKind.Request;
                return true;
            default:
                messageKind = default;
                return false;
        }
    }
}