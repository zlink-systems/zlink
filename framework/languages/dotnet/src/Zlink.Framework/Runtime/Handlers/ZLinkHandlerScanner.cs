using System.Reflection;

namespace Zlink.Framework.Runtime.Handlers;

internal static class ZLinkHandlerScanner
{
    public static IReadOnlyList<ZLinkHandlerEndpointDescriptor> Scan(Assembly assembly)
    {
        var endpoints = new List<ZLinkHandlerEndpointDescriptor>();

        foreach (var type in assembly.GetTypes())
        {
            if (type.IsAbstract || type.IsInterface) continue;

            var groups = ResolveGroups(type);
            foreach (var method in type.GetMethods(BindingFlags.Instance | BindingFlags.Public))
            foreach (var attribute in EnumerateEndpointAttributes(method))
                endpoints.Add(CreateDescriptor(
                    type,
                    method,
                    attribute.PacketName,
                    attribute.Kind,
                    groups));

            foreach (var iface in type.GetInterfaces())
            {
                if (!iface.IsGenericType) continue;

                var def = iface.GetGenericTypeDefinition();
                if (def == typeof(IZLinkRequestHandler<,>))
                    endpoints.Add(CreateInterfaceDescriptor(
                        type,
                        iface,
                        ZLinkMessageKind.Request,
                        groups,
                        null,
                        null));
                else if (def == typeof(IZLinkSendHandler<>))
                    endpoints.Add(CreateInterfaceDescriptor(
                        type,
                        iface,
                        ZLinkMessageKind.Command,
                        groups,
                        null,
                        null));
                else if (def == typeof(IZLinkFanoutHandler<>))
                    endpoints.Add(CreateInterfaceDescriptor(
                        type,
                        iface,
                        ZLinkMessageKind.Publish,
                        groups,
                        null,
                        null));
            }
        }

        return endpoints;
    }

    public static ZLinkHandlerEndpointDescriptor CreateExplicitInterfaceDescriptor(
        Type declaringType,
        Type handlerInterface,
        ZLinkMessageKind kind,
        string channelName,
        string? packetName)
    {
        return CreateInterfaceDescriptor(
            declaringType,
            handlerInterface,
            kind,
            new HashSet<string>(StringComparer.Ordinal),
            channelName,
            packetName);
    }

    public static ZLinkRouteHandlerEndpointDescriptor CreateExplicitRouteInterfaceDescriptor(
        Type declaringType,
        Type handlerInterface,
        ZLinkMessageKind kind,
        string? packetName)
    {
        return CreateRouteInterfaceDescriptor(
            declaringType,
            handlerInterface,
            kind,
            new HashSet<string>(StringComparer.Ordinal),
            packetName);
    }

    private static IEnumerable<ZLinkEndpointAttributeDescriptor> EnumerateEndpointAttributes(MethodInfo method)
    {
        if (method.GetCustomAttribute<ZLinkRequestAttribute>() is { } request)
            yield return new ZLinkEndpointAttributeDescriptor(
                ZLinkMessageKind.Request,
                request.PacketName);

        if (method.GetCustomAttribute<ZLinkSendAttribute>() is { } send)
            yield return new ZLinkEndpointAttributeDescriptor(
                ZLinkMessageKind.Command,
                send.PacketName);

        if (method.GetCustomAttribute<ZLinkPublishAttribute>() is { } publish)
            yield return new ZLinkEndpointAttributeDescriptor(
                ZLinkMessageKind.Publish,
                publish.PacketName);
    }

    private static IReadOnlySet<string> ResolveGroups(Type declaringType)
    {
        return declaringType
            .GetCustomAttributes<ZLinkHandlerGroupAttribute>()
            .Select(static attribute => attribute.GroupName)
            .Where(static group => !string.IsNullOrWhiteSpace(group))
            .ToHashSet(StringComparer.Ordinal);
    }

    private static ZLinkHandlerEndpointDescriptor CreateInterfaceDescriptor(
        Type declaringType,
        Type handlerInterface,
        ZLinkMessageKind kind,
        IReadOnlySet<string> groups,
        string? explicitChannelName,
        string? packetName)
    {
        return ZLinkHandlerEndpointDescriptorFactory.CreateInterface(
            declaringType,
            handlerInterface,
            kind,
            groups,
            explicitChannelName,
            packetName);
    }

    private static ZLinkRouteHandlerEndpointDescriptor CreateRouteInterfaceDescriptor(
        Type declaringType,
        Type handlerInterface,
        ZLinkMessageKind kind,
        IReadOnlySet<string> groups,
        string? packetName)
    {
        return ZLinkHandlerEndpointDescriptorFactory.CreateRouteInterface(
            declaringType,
            handlerInterface,
            kind,
            groups,
            packetName);
    }

    private static ZLinkHandlerEndpointDescriptor CreateDescriptor(
        Type declaringType,
        MethodInfo method,
        string? messageNameOverride,
        ZLinkMessageKind kind,
        IReadOnlySet<string> groups)
    {
        return ZLinkHandlerEndpointDescriptorFactory.CreateAttributed(
            declaringType,
            method,
            messageNameOverride,
            kind,
            groups);
    }

    private readonly record struct ZLinkEndpointAttributeDescriptor(
        ZLinkMessageKind Kind,
        string? PacketName);
}
