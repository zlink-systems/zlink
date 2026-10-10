namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotNativeDispatchRouter
{
    public static void Attach(
        IZLinkBackendSpot nativeSpot,
        Func<IReadOnlyList<ZLinkBackendRouteReceived>, ValueTask> routeReadable,
        Action<Action?> channelReplyReadable,
        Func<ValueTask> subscribeReadable,
        Action actorJoinReadable,
        Action actorLifecycleReadable,
        Action<
            Zlink.Framework.Runtime.Dispatch.ZLinkApplicationJobOrigin,
            IReadOnlyList<ZLinkBackendActorPart>,
            IDisposable?
        > actorPartsReadable
    )
    {
        try
        {
            nativeSpot.OnDispatchEvent(info =>
            {
                switch (info.Event)
                {
                    // Route admission runs on the Spot state lane; the dispatch
                    // worker keeps its completion instead of waiting for it.
                    case ZLinkBackendSpotDispatchEvent.RouteReadable:
                        return (routeReadable(info.RoutedMessages ?? []), null);
                    case ZLinkBackendSpotDispatchEvent.ChannelReplyReadable:
                        channelReplyReadable(info.DrainChannelReply);
                        break;
                    case ZLinkBackendSpotDispatchEvent.SubscribeReadable:
                        return (subscribeReadable(), null);
                    case ZLinkBackendSpotDispatchEvent.ActorJoinReadable:
                        actorJoinReadable();
                        break;
                    case ZLinkBackendSpotDispatchEvent.ActorLifecycleReadable:
                        actorLifecycleReadable();
                        break;
                    case ZLinkBackendSpotDispatchEvent.ActorReadable
                        when info.ActorParts is { Count: > 0 } actorParts:
                        actorPartsReadable(
                            info.ApplicationJobOrigin!.Value,
                            actorParts,
                            info.ActorPayloadOwner
                        );
                        break;
                }
                return (ValueTask.CompletedTask, null);
            });
        }
        catch (ZlinkHandlerException error)
            when (error.Result == ZlinkHandlerException.ErrorCode.NotSupported) { }
    }
}
