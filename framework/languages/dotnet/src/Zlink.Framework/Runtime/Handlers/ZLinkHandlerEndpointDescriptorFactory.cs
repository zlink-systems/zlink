using System.Reflection;

namespace Zlink.Framework.Runtime.Handlers;

internal static class ZLinkHandlerEndpointDescriptorFactory
{
    public static ZLinkHandlerEndpointDescriptor CreateInterface(
        Type declaringType,
        Type handlerInterface,
        ZLinkMessageKind kind,
        IReadOnlySet<string> groups,
        string? explicitChannelName,
        string? packetName)
    {
        var args = handlerInterface.GetGenericArguments();
        var messageType = args[0];
        var replyType = kind == ZLinkMessageKind.Request ? args[1] : null;
        var targetMethod = ResolveInterfaceHandleMethod(
            declaringType,
            handlerInterface,
            nameof(IZLinkFanoutHandler<object>.HandleAsync),
            "Handler");

        var messageName = packetName ?? ZLinkMessageNameResolver.ResolveFromType(messageType);
        Type? contextType = kind is ZLinkMessageKind.Request or ZLinkMessageKind.Command
            ? typeof(IZLinkMessageContext)
            : null;

        return new ZLinkHandlerEndpointDescriptor(
            kind,
            messageName,
            declaringType,
            ZLinkHandlerMethodInvokerFactory.Create(targetMethod),
            BuildArgumentPlan(targetMethod.GetParameters(), contextType),
            messageType,
            replyType,
            contextType,
            true,
            groups,
            explicitChannelName);
    }

    public static ZLinkRouteHandlerEndpointDescriptor CreateRouteInterface(
        Type declaringType,
        Type handlerInterface,
        ZLinkMessageKind kind,
        IReadOnlySet<string> groups,
        string? packetName)
    {
        var args = handlerInterface.GetGenericArguments();
        var messageType = args[0];
        var replyType = kind == ZLinkMessageKind.Request ? args[1] : null;
        var targetMethod = ResolveInterfaceHandleMethod(
            declaringType,
            handlerInterface,
            nameof(IZLinkRouteSendHandler<object>.HandleAsync),
            "Route handler");

        return new ZLinkRouteHandlerEndpointDescriptor(
            kind,
            packetName ?? ZLinkMessageNameResolver.ResolveFromType(messageType),
            declaringType,
            ZLinkHandlerMethodInvokerFactory.Create(targetMethod),
            messageType,
            replyType,
            groups);
    }

    public static ZLinkHandlerEndpointDescriptor CreateAttributed(
        Type declaringType,
        MethodInfo method,
        string? messageNameOverride,
        ZLinkMessageKind kind,
        IReadOnlySet<string> groups)
    {
        var parameters = method.GetParameters();
        if (parameters.Length == 0)
            throw new ZLinkConfigurationException(
                $"Handler method '{declaringType.FullName}.{method.Name}' must accept a message parameter.");

        var messageType = parameters[0].ParameterType;
        var messageName = messageNameOverride ?? ZLinkMessageNameResolver.ResolveFromType(messageType);
        Type? contextType = null;
        var hasCancellationToken = false;

        for (var i = 1; i < parameters.Length; i++)
        {
            if (parameters[i].ParameterType == typeof(CancellationToken))
            {
                hasCancellationToken = true;
                continue;
            }

            if (typeof(IZLinkMessageContext).IsAssignableFrom(parameters[i].ParameterType))
                contextType = parameters[i].ParameterType;
        }

        var replyType = kind == ZLinkMessageKind.Request
            ? GetReplyType(method.ReturnType)
            : null;

        return new ZLinkHandlerEndpointDescriptor(
            kind,
            messageName,
            declaringType,
            ZLinkHandlerMethodInvokerFactory.Create(method),
            BuildArgumentPlan(parameters, contextType),
            messageType,
            replyType,
            contextType,
            hasCancellationToken,
            groups,
            null);
    }

    private static MethodInfo ResolveInterfaceHandleMethod(
        Type declaringType,
        Type handlerInterface,
        string methodName,
        string label)
    {
        var map = declaringType.GetInterfaceMap(handlerInterface);
        for (var i = 0; i < map.InterfaceMethods.Length; i++)
            if (map.InterfaceMethods[i].Name == methodName)
                return map.TargetMethods[i];

        throw new ZLinkConfigurationException(
            $"{label} '{declaringType.FullName}' does not implement HandleAsync for '{handlerInterface.Name}'.");
    }

    private static ZLinkHandlerArgumentKind[] BuildArgumentPlan(
        IReadOnlyList<ParameterInfo> parameters,
        Type? contextType)
    {
        var plan = new ZLinkHandlerArgumentKind[parameters.Count];
        if (parameters.Count == 0) return plan;

        plan[0] = ZLinkHandlerArgumentKind.Message;
        for (var i = 1; i < parameters.Count; i++)
        {
            if (parameters[i].ParameterType == typeof(CancellationToken))
            {
                plan[i] = ZLinkHandlerArgumentKind.CancellationToken;
                continue;
            }

            if (contextType is not null && parameters[i].ParameterType.IsAssignableFrom(contextType))
            {
                plan[i] = ZLinkHandlerArgumentKind.Context;
                continue;
            }

            plan[i] = ZLinkHandlerArgumentKind.Default;
        }

        return plan;
    }

    private static Type? GetReplyType(Type returnType)
    {
        if (returnType.IsGenericType && returnType.GetGenericTypeDefinition() == typeof(ValueTask<>))
            return returnType.GetGenericArguments()[0];

        if (returnType.IsGenericType && returnType.GetGenericTypeDefinition() == typeof(Task<>))
            return returnType.GetGenericArguments()[0];

        if (returnType == typeof(ValueTask) || returnType == typeof(Task))
            throw new ZLinkConfigurationException("Request handlers must return a reply value.");

        return returnType;
    }
}
