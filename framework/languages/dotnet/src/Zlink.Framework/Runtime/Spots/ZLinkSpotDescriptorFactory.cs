using System.Reflection;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotDescriptorFactory
{
    private const string ActorJoinMethodName = "OnActorJoinAsync";

    public static ZLinkSpotDescriptor CreatePacketDescriptor(
        Type handlerType,
        Type expectedSpotType,
        string? packetName = null)
    {
        foreach (var (definition, arguments) in ZLinkHandlerContractInspector.EnumerateGenericInterfaces(handlerType))
        {
            if (definition == typeof(IZLinkSpotPacketHandler<,>))
            {
                ValidateSpotType(handlerType, expectedSpotType, arguments[0]);
                return new ZLinkSpotDescriptor
                {
                    HandlerType = handlerType,
                    SpotType = arguments[0],
                    MessageType = arguments[1],
                    Invoker = CreateInvoker(handlerType),
                    MessageName = packetName ?? ZLinkMessageNameResolver.ResolveFromType(arguments[1])
                };
            }

            if (definition == typeof(IZLinkSpotRequestHandler<,,>))
            {
                ValidateSpotType(handlerType, expectedSpotType, arguments[0]);
                return new ZLinkSpotDescriptor
                {
                    HandlerType = handlerType,
                    SpotType = arguments[0],
                    MessageType = arguments[1],
                    ReplyType = arguments[2],
                    Invoker = CreateInvoker(handlerType),
                    MessageName = packetName ?? ZLinkMessageNameResolver.ResolveFromType(arguments[1])
                };
            }
        }

        throw new InvalidOperationException(
            $"SPOT packet handler '{handlerType}' must implement IZLinkSpotPacketHandler<,> or IZLinkSpotRequestHandler<,,>.");
    }

    public static ZLinkSpotDescriptor CreateAttributedRequestDescriptor(
        Type spotType,
        MethodInfo method,
        string? packetName)
    {
        ValidateAttributedOwner(spotType, method, "SPOT request");
        var (messageType, passCancellationToken) = ValidatePayloadMethod(method, "SPOT request");
        var replyType = ResolveAsyncResult(method, "SPOT request", requireResult: true);
        return new ZLinkSpotDescriptor
        {
            HandlerType = spotType,
            SpotType = spotType,
            MessageType = messageType,
            ReplyType = replyType,
            Invoker = ZLinkHandlerMethodInvokerFactory.Create(method),
            MessageName = packetName ?? ZLinkMessageNameResolver.ResolveFromType(messageType),
            IsAttributed = true,
            PassCancellationToken = passCancellationToken
        };
    }

    public static ZLinkSpotSubscriptionDescriptor CreateSubscriptionDescriptor(
        string channelName,
        string topic,
        Type handlerType,
        Type expectedSpotType)
    {
        foreach (var (definition, arguments) in ZLinkHandlerContractInspector.EnumerateGenericInterfaces(handlerType))
        {
            if (definition != typeof(IZLinkSpotSubscriptionHandler<,>)) continue;

            ValidateSpotType(handlerType, expectedSpotType, arguments[0]);
            return new ZLinkSpotSubscriptionDescriptor
            {
                ChannelName = channelName,
                Topic = topic,
                HandlerType = handlerType,
                SpotType = arguments[0],
                MessageType = arguments[1],
                Invoker = CreateInvoker(handlerType),
                MessageName = ZLinkMessageNameResolver.ResolveFromType(arguments[1])
            };
        }

        throw new InvalidOperationException(
            $"SPOT subscription handler '{handlerType}' must implement IZLinkSpotSubscriptionHandler<,>.");
    }

    public static ZLinkSpotSubscriptionDescriptor CreateAttributedSubscriptionDescriptor(
        string channelName,
        string topic,
        Type spotType,
        MethodInfo method)
    {
        ValidateAttributedOwner(spotType, method, "SPOT subscription");
        var (messageType, passCancellationToken) = ValidatePayloadMethod(method, "SPOT subscription");
        _ = ResolveAsyncResult(method, "SPOT subscription", requireResult: false);
        return new ZLinkSpotSubscriptionDescriptor
        {
            ChannelName = channelName,
            Topic = topic,
            HandlerType = spotType,
            SpotType = spotType,
            MessageType = messageType,
            Invoker = ZLinkHandlerMethodInvokerFactory.Create(method),
            MessageName = ZLinkMessageNameResolver.ResolveFromType(messageType),
            IsAttributed = true,
            PassCancellationToken = passCancellationToken
        };
    }

    private static void ValidateAttributedOwner(Type spotType, MethodInfo method, string kind)
    {
        if (!typeof(IZLinkSpot).IsAssignableFrom(spotType) || method.DeclaringType != spotType)
            throw new ZLinkConfigurationException(
                $"{kind} method '{method.DeclaringType?.FullName}.{method.Name}' must be declared by a concrete IZLinkSpot type.");
        if (method.IsStatic || method.IsGenericMethodDefinition)
            throw new ZLinkConfigurationException($"{kind} method '{spotType.FullName}.{method.Name}' must be a non-generic instance method.");
    }

    private static (Type MessageType, bool PassCancellationToken) ValidatePayloadMethod(
        MethodInfo method,
        string kind)
    {
        var parameters = method.GetParameters();
        if (parameters.Length is < 1 or > 2
            || (parameters.Length == 2 && parameters[1].ParameterType != typeof(CancellationToken)))
            throw new ZLinkConfigurationException(
                $"{kind} method '{method.DeclaringType?.FullName}.{method.Name}' must accept a payload followed by an optional CancellationToken.");
        if (parameters[0].ParameterType.IsByRef)
            throw new ZLinkConfigurationException($"{kind} payload must not use ref, in, or out.");
        return (parameters[0].ParameterType, parameters.Length == 2);
    }

    private static Type? ResolveAsyncResult(MethodInfo method, string kind, bool requireResult)
    {
        var returnType = method.ReturnType;
        if (returnType == typeof(Task) || returnType == typeof(ValueTask))
        {
            if (requireResult)
                throw new ZLinkConfigurationException($"{kind} method '{method.DeclaringType?.FullName}.{method.Name}' must return Task<TReply> or ValueTask<TReply>.");
            return null;
        }

        if (returnType.IsGenericType
            && (returnType.GetGenericTypeDefinition() == typeof(Task<>)
                || returnType.GetGenericTypeDefinition() == typeof(ValueTask<>)))
        {
            if (!requireResult)
                throw new ZLinkConfigurationException($"{kind} method '{method.DeclaringType?.FullName}.{method.Name}' must return Task or ValueTask.");
            return returnType.GetGenericArguments()[0];
        }

        throw new ZLinkConfigurationException(
            $"{kind} method '{method.DeclaringType?.FullName}.{method.Name}' must use an asynchronous Task or ValueTask return type.");
    }

    public static ZLinkSpotTimerDescriptor CreateTimerDescriptor(
        string name,
        TimeSpan period,
        Type handlerType,
        Type expectedSpotType)
    {
        foreach (var (definition, arguments) in ZLinkHandlerContractInspector.EnumerateGenericInterfaces(handlerType))
        {
            if (definition != typeof(IZLinkSpotTimerHandler<>)) continue;

            ValidateSpotType(handlerType, expectedSpotType, arguments[0]);
            return new ZLinkSpotTimerDescriptor
            {
                Name = name,
                Period = period,
                HandlerType = handlerType,
                SpotType = arguments[0],
                Invoker = CreateInvoker(handlerType)
            };
        }

        throw new InvalidOperationException(
            $"SPOT timer handler '{handlerType}' must implement IZLinkSpotTimerHandler<>.");
    }

    public static IEnumerable<ZLinkSpotActorJoinDescriptor> CreateSpotActorJoinDescriptors(Type spotType)
    {
        var contract = ZLinkSpotActorContractInspector.GetSpotOrEntryContract(spotType);
        foreach (var method in EnumerateActorJoinMethods(spotType, contract?.ContractType))
        {
            if (contract is null)
                throw new InvalidOperationException(
                    $"SPOT actor join hook '{spotType}' must implement IZLinkSpot<TActor> or IZLinkEntrySpot<TActor>.");

            yield return CreateSpotActorJoinDescriptor(spotType, method, contract.ActorType);
        }
    }

    private static void ValidateSpotType(Type handlerType, Type expectedSpotType, Type actualSpotType)
    {
        ZLinkHandlerContractDescriptorSupport.RequireExactType(
            handlerType,
            expectedSpotType,
            actualSpotType,
            "SPOT handler");
    }

    private static void ValidateActorType(Type handlerType, Type? expectedActorType, Type actualActorType)
    {
        if (expectedActorType is null)
        {
            if (!typeof(IZLinkActor).IsAssignableFrom(actualActorType))
                throw new InvalidOperationException(
                    $"SPOT actor join callback '{handlerType}' targets '{actualActorType}', but actor type must implement '{typeof(IZLinkActor)}'.");

            return;
        }

        ZLinkHandlerContractDescriptorSupport.RequireExactType(
            handlerType,
            expectedActorType,
            actualActorType,
            "SPOT actor join callback");
    }

    private static ZLinkHandlerMethodInvoker CreateInvoker(Type handlerType)
    {
        return ZLinkHandlerContractDescriptorSupport.CreateHandleAsyncInvoker(handlerType, "Handler");
    }

    private static ZLinkSpotActorJoinDescriptor CreateSpotActorJoinDescriptor(
        Type spotType,
        MethodInfo method,
        Type expectedActorType)
    {
        var parameters = ZLinkHandlerMethodShape.RequireParameterCount(
            spotType,
            method,
            3,
            "SPOT actor join hook");
        if (parameters[0].ParameterType != typeof(string))
            throw new InvalidOperationException(
                $"SPOT actor join hook '{spotType}' method '{method.Name}' must use string actorId as the first parameter.");

        if (parameters[1].ParameterType != typeof(ZLinkMessage))
            throw new InvalidOperationException(
                $"SPOT actor join hook '{spotType}' method '{method.Name}' must use ZLinkMessage as the second parameter.");

        ZLinkHandlerMethodShape.RequireCancellationToken(
            spotType,
            method,
            parameters[2],
            "SPOT actor join hook",
            "third");
        if (method.ReturnType != typeof(ValueTask<ZLinkSpotActorJoinResult>))
            throw new InvalidOperationException(
                $"SPOT actor join hook '{spotType}' method '{method.Name}' must return ValueTask<ZLinkSpotActorJoinResult>.");

        return new ZLinkSpotActorJoinDescriptor
        {
            HandlerType = spotType,
            SpotType = spotType,
            ActorType = expectedActorType,
            Invoker = ZLinkHandlerMethodInvokerFactory.Create(method),
            PassSpotArgument = false
        };
    }

    private static IEnumerable<MethodInfo> EnumerateActorJoinMethods(Type spotType, Type? contractType)
    {
        var declaredMethods = spotType
            .GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .Where(method => method.Name == ActorJoinMethodName
                             && method.DeclaringType == spotType)
            .ToArray();
        if (declaredMethods.Length > 0) return declaredMethods;

        return contractType is null
            ? []
            : ZLinkSpotActorContractInspector.EnumerateInterfaceMethods(contractType)
                .Where(method => method.Name == ActorJoinMethodName);
    }
}
