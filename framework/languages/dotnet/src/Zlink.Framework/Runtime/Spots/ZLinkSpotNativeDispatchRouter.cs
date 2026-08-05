using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotNativeDispatchRouter
{
    public static void Attach(
        IZLinkBackendSpot nativeSpot,
        Action<IReadOnlyList<ZLinkBackendRouteReceived>> routeReadable,
        Action<Action?> channelReplyReadable,
        Action subscribeReadable,
        Action actorJoinReadable,
        Action actorLifecycleReadable,
        Action<IReadOnlyList<ZLinkBackendActorPart>, ZLinkInboundDispatchLease?> actorPartsReadable)
    {
        try
        {
            nativeSpot.OnDispatchEvent(info =>
            {
                switch (info.Event)
                {
                    case ZLinkBackendSpotDispatchEvent.RouteReadable:
                        routeReadable(info.RoutedMessages ?? []);
                        break;
                    case ZLinkBackendSpotDispatchEvent.ChannelReplyReadable:
                        channelReplyReadable(info.DrainChannelReply);
                        break;
                    case ZLinkBackendSpotDispatchEvent.SubscribeReadable:
                        subscribeReadable();
                        break;
                    case ZLinkBackendSpotDispatchEvent.ActorJoinReadable:
                        actorJoinReadable();
                        break;
                    case ZLinkBackendSpotDispatchEvent.ActorLifecycleReadable:
                        actorLifecycleReadable();
                        break;
                    case ZLinkBackendSpotDispatchEvent.ActorReadable
                        when info.ActorParts is { Count: > 0 } actorParts:
                        actorPartsReadable(actorParts, info.ActorDispatchLease);
                        break;
                }
            });
        }
        catch (ZlinkHandlerException error)
            when (error.Result == ZlinkHandlerException.ErrorCode.NotSupported)
        {
        }
    }
}
