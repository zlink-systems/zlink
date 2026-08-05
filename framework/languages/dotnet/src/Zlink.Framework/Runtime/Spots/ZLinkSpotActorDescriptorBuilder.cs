namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotActorDescriptorBuilder
{
    public static ZLinkSpotActorPacketDescriptor CreatePacket(
        ZLinkSpotActorHandlerSurface surface,
        Type handlerType,
        Type? spotType,
        Type actorType,
        Type messageType,
        Type? replyType,
        string? packetName)
    {
        return CreatePacket(
            surface,
            handlerType,
            spotType,
            actorType,
            messageType,
            replyType,
            packetName,
            CreateInterfaceInvoker(handlerType));
    }

    public static ZLinkSpotActorPacketDescriptor CreatePacket(
        ZLinkSpotActorHandlerSurface surface,
        Type handlerType,
        Type? spotType,
        Type actorType,
        Type messageType,
        Type? replyType,
        string? packetName,
        ZLinkHandlerMethodInvoker invoker)
    {
        return new ZLinkSpotActorPacketDescriptor
        {
            HandlerType = handlerType,
            SpotType = spotType,
            ActorType = actorType,
            MessageType = messageType,
            ReplyType = replyType,
            Kind = replyType is null ? ZLinkMessageKind.Command : ZLinkMessageKind.Request,
            Invoker = invoker,
            MessageName = packetName ?? ZLinkMessageNameResolver.ResolveFromType(messageType),
            Surface = surface
        };
    }

    public static ZLinkSpotActorLifecycleDescriptor CreateLifecycle(
        ZLinkSpotActorHandlerSurface surface,
        Type handlerType,
        Type spotType,
        Type actorType,
        ZLinkHandlerMethodInvoker invoker,
        bool passRequestArgument = false)
    {
        return new ZLinkSpotActorLifecycleDescriptor
        {
            HandlerType = handlerType,
            SpotType = spotType,
            ActorType = actorType,
            Invoker = invoker,
            Surface = surface,
            PassSpotArgument = false,
            PassRequestArgument = passRequestArgument
        };
    }

    public static void ValidateSpotType(
        Type handlerType,
        Type? expectedSpotType,
        Type actualSpotType)
    {
        if (expectedSpotType is null)
            throw new InvalidOperationException(
                $"SPOT actor handler '{handlerType}' targets SPOT '{actualSpotType}', but registration expects '{expectedSpotType}'.");

        ZLinkHandlerContractDescriptorSupport.RequireExactType(
            handlerType,
            expectedSpotType,
            actualSpotType,
            "SPOT actor handler");
    }

    public static void ValidateActorType(
        Type handlerType,
        Type? expectedActorType,
        Type actualActorType)
    {
        if (expectedActorType is null) return;

        ZLinkHandlerContractDescriptorSupport.RequireAssignableFrom(
            handlerType,
            expectedActorType,
            actualActorType,
            "SPOT actor handler");
    }

    public static Type GetRequestReplyType(Type returnType)
    {
        return ZLinkHandlerMethodShape.RequireReplyType(returnType, "SPOT actor request handler");
    }

    private static ZLinkHandlerMethodInvoker CreateInterfaceInvoker(Type handlerType)
    {
        return ZLinkHandlerContractDescriptorSupport.CreateHandleAsyncInvoker(handlerType, "Handler");
    }
}
