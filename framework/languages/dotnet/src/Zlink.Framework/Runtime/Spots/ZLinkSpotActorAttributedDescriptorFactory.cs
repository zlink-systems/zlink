using System.Reflection;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotActorAttributedDescriptorFactory
{
    private const string ActorCreatedMethodName = "OnCreateActorAsync";
    private const string PostActorJoinedMethodName = "OnJoinedActorAsync";
    private const string ActorLeftMethodName = "OnLeaveActorAsync";
    private const string ActorDisconnectedMethodName = "OnDisconnectActorAsync";

    public static IEnumerable<ZLinkSpotActorPacketDescriptor> CreatePacketDescriptors(
        ZLinkSpotActorHandlerSurface surface,
        Type? expectedSpotType,
        Type handlerType,
        Type? expectedActorType,
        string? packetNameOverride)
    {
        foreach (var method in EnumeratePacketMethods(handlerType))
        {
            var descriptor = TryCreatePacket(
                surface,
                expectedSpotType,
                handlerType,
                expectedActorType,
                method,
                packetNameOverride);
            if (descriptor is not null) yield return descriptor;
        }
    }

    public static IEnumerable<ZLinkSpotActorInferredHandlerDescriptor> CreateInferredDescriptors(
        ZLinkSpotActorHandlerSurface surface,
        Type? expectedSpotType,
        Type handlerType,
        string? packetName)
    {
        foreach (var descriptor in CreatePacketDescriptors(surface, expectedSpotType, handlerType, null, packetName))
            yield return new ZLinkSpotActorInferredHandlerDescriptor { Packet = descriptor };
    }

    public static IEnumerable<ZLinkSpotActorInferredHandlerDescriptor> CreateSpotLifecycleDescriptors(
        ZLinkSpotActorHandlerSurface surface,
        Type spotType)
    {
        var contract = ZLinkSpotActorContractInspector.GetSurfaceContract(surface, spotType);
        foreach (var method in EnumerateSpotLifecycleMethods(spotType, contract?.ContractType))
            if (method.Name == ActorCreatedMethodName)
            {
                if (contract is null)
                    throw new InvalidOperationException(
                        $"SPOT actor lifecycle hook '{spotType}' must implement IZLinkEntrySpot<TActor>.");

                if (surface != ZLinkSpotActorHandlerSurface.EntrySpot)
                    throw new InvalidOperationException(
                        $"SPOT actor lifecycle hook '{spotType}' method '{ActorCreatedMethodName}' is only valid on Entry Spot.");

                yield return new ZLinkSpotActorInferredHandlerDescriptor
                {
                    Created = CreateSpotLifecycle(
                        surface,
                        spotType,
                        method,
                        contract.ActorType,
                        true)
                };
            }
            else if (method.Name == PostActorJoinedMethodName)
            {
                if (contract is null)
                    throw new InvalidOperationException(
                        $"SPOT actor lifecycle hook '{spotType}' must implement IZLinkSpot<TActor> or IZLinkEntrySpot<TActor>.");

                yield return new ZLinkSpotActorInferredHandlerDescriptor
                {
                    Joined = CreateSpotLifecycle(surface, spotType, method, contract.ActorType)
                };
            }
            else if (method.Name == ActorLeftMethodName)
            {
                if (contract is null)
                    throw new InvalidOperationException(
                        $"SPOT actor lifecycle hook '{spotType}' must implement IZLinkSpot<TActor> or IZLinkEntrySpot<TActor>.");

                yield return new ZLinkSpotActorInferredHandlerDescriptor
                {
                    Left = CreateSpotLifecycle(surface, spotType, method, contract.ActorType)
                };
            }
            else if (method.Name == ActorDisconnectedMethodName)
            {
                if (contract is null)
                    throw new InvalidOperationException(
                        $"SPOT actor lifecycle hook '{spotType}' must implement IZLinkSpot<TActor> or IZLinkEntrySpot<TActor>.");

                yield return new ZLinkSpotActorInferredHandlerDescriptor
                {
                    Disconnected = CreateSpotLifecycle(surface, spotType, method, contract.ActorType)
                };
            }
    }

    private static ZLinkSpotActorPacketDescriptor? TryCreatePacket(
        ZLinkSpotActorHandlerSurface surface,
        Type? expectedSpotType,
        Type handlerType,
        Type? expectedActorType,
        MethodInfo method,
        string? packetNameOverride)
    {
        var send = method.GetCustomAttribute<ZLinkSpotActorSendAttribute>();
        var request = method.GetCustomAttribute<ZLinkSpotActorRequestAttribute>();
        if (send is not null && request is not null)
            throw new InvalidOperationException(
                $"SPOT actor handler '{handlerType}' method '{method.Name}' cannot declare both send and request attributes.");

        if (send is null && request is null) return null;

        var parameters =
            ZLinkHandlerMethodShape.RequireParameterCount(handlerType, method, 5, "SPOT actor packet handler");
        var spotType = parameters[0].ParameterType;
        var actorType = parameters[1].ParameterType;
        var expectedContextType = typeof(IZLinkMessageContext);
        if (parameters[2].ParameterType != expectedContextType)
            throw new InvalidOperationException(
                $"SPOT actor packet handler '{handlerType}' method '{method.Name}' must use {expectedContextType.Name} as the third parameter.");

        var messageType = parameters[3].ParameterType;
        ZLinkHandlerMethodShape.RequireCancellationToken(handlerType, method, parameters[4],
            "SPOT actor packet handler");
        ZLinkSpotActorDescriptorBuilder.ValidateSpotType(handlerType, expectedSpotType, spotType);
        ZLinkSpotActorDescriptorBuilder.ValidateActorType(handlerType, expectedActorType, actorType);
        var replyType = request is null
            ? null
            : ZLinkSpotActorDescriptorBuilder.GetRequestReplyType(method.ReturnType);
        if (send is not null) ZLinkHandlerMethodShape.RequireNoReply(handlerType, method, "SPOT actor send handler");

        var packetName = packetNameOverride ?? send?.PacketName ?? request?.PacketName;
        return ZLinkSpotActorDescriptorBuilder.CreatePacket(
            surface,
            handlerType,
            spotType,
            actorType,
            messageType,
            replyType,
            packetName,
            ZLinkHandlerMethodInvokerFactory.Create(method));
    }

    private static ZLinkSpotActorLifecycleDescriptor CreateSpotLifecycle(
        ZLinkSpotActorHandlerSurface surface,
        Type spotType,
        MethodInfo method,
        Type expectedActorType,
        bool passRequestArgument = false)
    {
        var parameters = ZLinkHandlerMethodShape.RequireParameterCount(
            spotType,
            method,
            passRequestArgument ? 3 : 2,
            "SPOT actor lifecycle hook");
        var actorType = parameters[0].ParameterType;
        if (passRequestArgument && parameters[1].ParameterType != typeof(ZLinkMessage))
            throw new InvalidOperationException(
                $"SPOT actor lifecycle hook '{spotType}' method '{method.Name}' must use {nameof(ZLinkMessage)} as the second parameter.");

        ZLinkHandlerMethodShape.RequireCancellationToken(
            spotType,
            method,
            parameters[passRequestArgument ? 2 : 1],
            "SPOT actor lifecycle hook",
            passRequestArgument ? "third" : "second");
        if (passRequestArgument)
        {
            var expectedValueTask = typeof(ValueTask<ZLinkActorCreateResponse>);
            var expectedTask = typeof(Task<ZLinkActorCreateResponse>);
            if (method.ReturnType != expectedValueTask
                && method.ReturnType != expectedTask)
            {
                throw new InvalidOperationException(
                    $"SPOT actor creation hook '{spotType}' method '{method.Name}' "
                    + $"must return {expectedValueTask.Name} or {expectedTask.Name}.");
            }
        }
        else
        {
            ZLinkHandlerMethodShape.RequireNoReply(
                spotType,
                method,
                "SPOT actor lifecycle hook");
        }
        ZLinkSpotActorDescriptorBuilder.ValidateActorType(spotType, expectedActorType, actorType);
        return ZLinkSpotActorDescriptorBuilder.CreateLifecycle(
            surface,
            spotType,
            spotType,
            actorType,
            ZLinkHandlerMethodInvokerFactory.Create(method),
            passRequestArgument);
    }

    private static IEnumerable<MethodInfo> EnumerateSpotLifecycleMethods(Type spotType, Type? contractType)
    {
        var declaredMethods = spotType
            .GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .Where(method => method.DeclaringType == spotType
                             && (method.Name == ActorCreatedMethodName
                                 || method.Name == PostActorJoinedMethodName
                                 || method.Name == ActorLeftMethodName
                                 || method.Name == ActorDisconnectedMethodName))
            .ToArray();

        foreach (var method in EnumerateLifecycleMethod(
                     declaredMethods,
                     contractType,
                     ActorCreatedMethodName))
            yield return method;

        foreach (var method in EnumerateLifecycleMethod(
                     declaredMethods,
                     contractType,
                     PostActorJoinedMethodName))
            yield return method;

        foreach (var method in EnumerateLifecycleMethod(
                     declaredMethods,
                     contractType,
                     ActorLeftMethodName))
            yield return method;

        foreach (var method in EnumerateLifecycleMethod(
                     declaredMethods,
                     contractType,
                     ActorDisconnectedMethodName))
            yield return method;
    }

    private static IEnumerable<MethodInfo> EnumerateLifecycleMethod(
        IReadOnlyList<MethodInfo> declaredMethods,
        Type? contractType,
        string methodName)
    {
        var declared = declaredMethods
            .Where(method => method.Name == methodName)
            .ToArray();
        if (declared.Length > 0) return declared;

        return contractType is null
            ? []
            : ZLinkSpotActorContractInspector.EnumerateInterfaceMethods(contractType)
                .Where(method => method.Name == methodName);
    }

    private static IEnumerable<MethodInfo> EnumeratePacketMethods(Type handlerType)
    {
        return handlerType
            .GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .Where(static method =>
                method.GetCustomAttribute<ZLinkSpotActorSendAttribute>() is not null
                || method.GetCustomAttribute<ZLinkSpotActorRequestAttribute>() is not null);
    }

}
