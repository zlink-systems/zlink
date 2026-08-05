namespace Zlink.Framework.Runtime.Channels;

internal sealed record ZLinkRouteHandlerDescriptor(
    ZLinkMessageKind Kind,
    string RouterChannelId,
    string PacketName,
    Type HandlerType,
    Type MessageType,
    Type? ReplyType,
    ZLinkHandlerMethodInvoker Invoker);

internal sealed class ZLinkRouteHandlerRegistry(IEnumerable<ZLinkRouteHandlerDescriptor> descriptors)
{
    private readonly Dictionary<(string Channel, ZLinkMessageKind Kind, string Packet), ZLinkRouteHandlerDescriptor>
        _handlers =
            Build(descriptors);

    public bool TryGet(
        string routerChannelId,
        ZLinkMessageKind kind,
        string packetName,
        out ZLinkRouteHandlerDescriptor? descriptor)
    {
        return _handlers.TryGetValue((routerChannelId, kind, packetName), out descriptor);
    }

    private static Dictionary<(string Channel, ZLinkMessageKind Kind, string Packet), ZLinkRouteHandlerDescriptor>
        Build(
            IEnumerable<ZLinkRouteHandlerDescriptor> descriptors)
    {
        var handlers =
            new Dictionary<(string Channel, ZLinkMessageKind Kind, string Packet), ZLinkRouteHandlerDescriptor>();
        foreach (var descriptor in descriptors)
            if (!handlers.TryAdd((descriptor.RouterChannelId, descriptor.Kind, descriptor.PacketName), descriptor))
                throw new ZLinkConfigurationException(
                    $"Duplicate routed handler '{descriptor.RouterChannelId}:{descriptor.Kind}:{descriptor.PacketName}'.");

        return handlers;
    }
}
