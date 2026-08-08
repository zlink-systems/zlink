using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Channels;

internal sealed record ZLinkRouteHandlerDescriptor(
    ZLinkMessageKind Kind,
    string RouterChannelId,
    string PacketName,
    Type HandlerType,
    Type MessageType,
    Type? ReplyType,
    ZLinkHandlerMethodInvoker Invoker);

// Node-direct handlers use an empty channel marker because they are addressed
// by RID rather than by a configured channel. Keep that sentinel distinct from
// a real channel name at the registry boundary.
internal readonly record struct ZLinkRouteHandlerChannelKey
{
    private readonly ZLinkChannelName _channel;
    private readonly bool _isNodeRoute;

    private ZLinkRouteHandlerChannelKey(
        ZLinkChannelName channel,
        bool isNodeRoute)
    {
        _channel = channel;
        _isNodeRoute = isNodeRoute;
    }

    internal static ZLinkRouteHandlerChannelKey FromBoundary(
        string value,
        string paramName) =>
        string.IsNullOrEmpty(value)
            ? new(default, isNodeRoute: true)
            : new(
                ZLinkChannelName.FromBoundary(value, paramName),
                isNodeRoute: false);
}

internal sealed class ZLinkRouteHandlerRegistry(IEnumerable<ZLinkRouteHandlerDescriptor> descriptors)
{
    private readonly Dictionary<(ZLinkRouteHandlerChannelKey Channel, ZLinkMessageKind Kind, string Packet), ZLinkRouteHandlerDescriptor>
        _handlers =
            Build(descriptors);

    public bool TryGet(
        string routerChannelId,
        ZLinkMessageKind kind,
        string packetName,
        out ZLinkRouteHandlerDescriptor? descriptor)
    {
        return _handlers.TryGetValue(
            (ZLinkRouteHandlerChannelKey.FromBoundary(
                 routerChannelId,
                 nameof(routerChannelId)),
             kind,
             packetName),
            out descriptor);
    }

    private static Dictionary<(ZLinkRouteHandlerChannelKey Channel, ZLinkMessageKind Kind, string Packet), ZLinkRouteHandlerDescriptor>
        Build(
            IEnumerable<ZLinkRouteHandlerDescriptor> descriptors)
    {
        var handlers = new Dictionary<
            (ZLinkRouteHandlerChannelKey Channel, ZLinkMessageKind Kind, string Packet),
            ZLinkRouteHandlerDescriptor>();
        foreach (var descriptor in descriptors)
        {
            var channel = ZLinkRouteHandlerChannelKey.FromBoundary(
                descriptor.RouterChannelId,
                nameof(descriptor));
            if (!handlers.TryAdd((channel, descriptor.Kind, descriptor.PacketName), descriptor))
                throw new ZLinkConfigurationException(
                    $"Duplicate routed handler '{descriptor.RouterChannelId}:{descriptor.Kind}:{descriptor.PacketName}'.");
        }

        return handlers;
    }
}
