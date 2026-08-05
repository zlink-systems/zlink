using System.Reflection;
using Zlink.Framework.Runtime.Handlers;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkSessionHandlerRegistry(ZLinkScopedHandlerInstanceOwner handlerInstances)
    : IZLinkSessionHandlerRegistry
{
    private readonly Dictionary<string, ZLinkSessionPacketHandlerDescriptor> _handlers =
        new(StringComparer.Ordinal);

    private bool _bound;
    private IZLinkSessionContext? _context;
    private IZLinkSession? _session;

    public void AddHandler<THandler>()
        where THandler : class
    {
        AddHandler(typeof(THandler), null);
    }

    public void AddHandler<THandler>(string packetName)
        where THandler : class
    {
        if (string.IsNullOrWhiteSpace(packetName)
            || !string.Equals(packetName, packetName.Trim(), StringComparison.Ordinal))
            throw new ZLinkConfigurationException(
                $"Session packet handler '{typeof(THandler).FullName}' must declare a non-empty packet name.");

        AddHandler(typeof(THandler), packetName);
    }

    public async ValueTask<bool> TryHandleAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken = default)
    {
        if (!_handlers.TryGetValue(dispatch.PacketName, out var handler)) return false;

        var context = _context
                      ?? throw new InvalidOperationException("Session handler registry is not bound to a session.");

        await handler.HandleAsync(
                handlerInstances,
                _session,
                context,
                dispatch,
                payload,
                cancellationToken)
            .ConfigureAwait(false);
        return true;
    }

    internal void BindContext(IZLinkSessionContext context)
    {
        if (_context is not null) throw new InvalidOperationException("Session handler registry is already bound.");

        _context = context;
    }

    internal void BindSession(IZLinkSession session)
    {
        if (_session is not null) throw new InvalidOperationException("Session handler owner is already bound.");
        _session = session;
    }

    internal void Bind()
    {
        _bound = true;
    }

    internal void AddScannedHandlers(IReadOnlyList<ZLinkScannedSessionHandler> candidates)
    {
        EnsureNotBound();
        var context = _context
                      ?? throw new InvalidOperationException("Session handler registry is not bound to a session.");
        var contextType = context.GetType();
        var sessionType = _session?.GetType();
        foreach (var candidate in candidates)
        {
            switch (candidate)
            {
                case ZLinkScannedAttributedSessionHandler attributed
                    when attributed.SessionType == sessionType:
                    AddDescriptor(attributed.Descriptor);
                    break;
                case ZLinkScannedInterfaceSessionHandler implemented
                    when implemented.ContextType.IsAssignableFrom(contextType):
                    AddDescriptor(implemented.Descriptor);
                    break;
            }
        }
    }

    private void AddHandler(Type handlerType, string? packetName)
    {
        EnsureNotBound();
        var context = _context
                      ?? throw new InvalidOperationException("Session handler registry is not bound to a session.");
        var descriptor = ZLinkSessionPacketHandlerDescriptorFactory.Create(
            handlerType,
            context.GetType(),
            packetName);

        AddDescriptor(descriptor);
    }

    private void AddDescriptor(ZLinkSessionPacketHandlerDescriptor descriptor)
    {
        if (_handlers.TryGetValue(descriptor.PacketName, out var existing)
            && existing.HandlerType == descriptor.HandlerType
            && existing.MessageType == descriptor.MessageType)
            return;

        if (existing is not null)
            throw new ZLinkConfigurationException(
                $"Session packet handler '{descriptor.PacketName}' is already registered.");

        _handlers.Add(descriptor.PacketName, descriptor);
    }

    private void EnsureNotBound()
    {
        if (_bound)
            throw new InvalidOperationException(
                "Session handler registration is only allowed while Configure is running.");
    }
}

internal sealed class ZLinkAttributedSessionPacketHandlerDescriptor<TMessage>(
    Type sessionType,
    string packetName,
    ZLinkHandlerMethodInvoker invoker,
    bool passDispatch,
    bool passCancellationToken)
    : ZLinkSessionPacketHandlerDescriptor(sessionType, typeof(TMessage), packetName)
{
    public override async ValueTask HandleAsync(
        ZLinkScopedHandlerInstanceOwner handlerInstances,
        IZLinkSession? session,
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        if (session is null || !HandlerType.IsInstanceOfType(session))
            throw new InvalidOperationException(
                $"Active session owner '{HandlerType.FullName}' is not available for attributed packet dispatch.");

        var message = payload.Decode<TMessage>();
        var argumentCount = 1 + (passDispatch ? 1 : 0) + (passCancellationToken ? 1 : 0);
        await ZLinkHandlerInvocationEngine.InvokeAsync(
                session,
                invoker,
                argumentCount,
                arguments =>
                {
                    var index = 0;
                    arguments[index++] = message;
                    if (passDispatch) arguments[index++] = dispatch;
                    if (passCancellationToken) arguments[index] = cancellationToken;
                })
            .ConfigureAwait(false);
    }
}

internal abstract class ZLinkSessionPacketHandlerDescriptor(
    Type handlerType,
    Type messageType,
    string packetName)
{
    public Type HandlerType { get; } = handlerType;

    public Type MessageType { get; } = messageType;

    public string PacketName { get; } = packetName;

    public abstract ValueTask HandleAsync(
        ZLinkScopedHandlerInstanceOwner handlerInstances,
        IZLinkSession? session,
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken);
}

internal abstract record ZLinkScannedSessionHandler(
    ZLinkSessionPacketHandlerDescriptor Descriptor);

internal sealed record ZLinkScannedAttributedSessionHandler(
    Type SessionType,
    ZLinkSessionPacketHandlerDescriptor Descriptor)
    : ZLinkScannedSessionHandler(Descriptor);

internal sealed record ZLinkScannedInterfaceSessionHandler(
    Type ContextType,
    ZLinkSessionPacketHandlerDescriptor Descriptor)
    : ZLinkScannedSessionHandler(Descriptor);

internal static class ZLinkScannedSessionHandlerScanner
{
    public static IReadOnlyList<ZLinkScannedSessionHandler> Scan(
        Assembly assembly,
        IReadOnlySet<Type> sessionTypes)
    {
        var candidates = new List<ZLinkScannedSessionHandler>();
        foreach (var handlerType in assembly.GetTypes())
        {
            if (handlerType.IsAbstract || handlerType.IsInterface) continue;

            if (sessionTypes.Contains(handlerType))
                foreach (var method in handlerType.GetMethods(BindingFlags.Instance | BindingFlags.Public))
                    if (method.GetCustomAttribute<ZLinkStreamPacketAttribute>() is not null)
                        candidates.Add(new ZLinkScannedAttributedSessionHandler(
                            handlerType,
                            ZLinkSessionPacketHandlerDescriptorFactory.CreateAttributed(handlerType, method)));

            foreach (var (definition, arguments) in ZLinkHandlerContractInspector.EnumerateGenericInterfaces(handlerType))
            {
                if (definition != typeof(IZLinkSessionPacketHandler<,>)) continue;

                candidates.Add(new ZLinkScannedInterfaceSessionHandler(
                    arguments[0],
                    ZLinkSessionPacketHandlerDescriptorFactory.Create(
                        handlerType,
                        arguments[0],
                        arguments[1],
                        null)));
            }
        }

        return Array.AsReadOnly(candidates.ToArray());
    }
}

internal sealed class ZLinkSessionPacketHandlerDescriptor<TSessionContext, TMessage, THandler>
    : ZLinkSessionPacketHandlerDescriptor
    where THandler : class, IZLinkSessionPacketHandler<TSessionContext, TMessage>
{
    public ZLinkSessionPacketHandlerDescriptor(string packetName)
        : base(typeof(THandler), typeof(TMessage), packetName)
    {
    }

    public override async ValueTask HandleAsync(
        ZLinkScopedHandlerInstanceOwner handlerInstances,
        IZLinkSession? session,
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        var handler = handlerInstances.Resolve<THandler>();
        await handler.HandleAsync(
                (TSessionContext)context,
                dispatch,
                payload.Decode<TMessage>(),
                cancellationToken)
            .ConfigureAwait(false);
    }
}

internal static class ZLinkSessionPacketHandlerDescriptorFactory
{
    public static ZLinkSessionPacketHandlerDescriptor CreateAttributed(Type sessionType, MethodInfo method)
    {
        if (!typeof(IZLinkSession).IsAssignableFrom(sessionType)
            || method.DeclaringType != sessionType
            || method.IsStatic
            || method.IsGenericMethodDefinition)
            throw new ZLinkConfigurationException(
                $"Stream packet method '{method.DeclaringType?.FullName}.{method.Name}' must be a non-generic instance method on the concrete IZLinkSession type.");

        var parameters = method.GetParameters();
        if (parameters.Length is < 1 or > 3)
            throw InvalidShape(sessionType, method);
        var messageType = parameters[0].ParameterType;
        var passDispatch = parameters.Length >= 2
                           && parameters[1].ParameterType == typeof(ZLinkSessionDispatchContext);
        var cancellationIndex = passDispatch ? 2 : 1;
        var passCancellationToken = parameters.Length > cancellationIndex
                                    && parameters[cancellationIndex].ParameterType == typeof(CancellationToken);
        if (parameters.Length != 1 + (passDispatch ? 1 : 0) + (passCancellationToken ? 1 : 0)
            || messageType.IsByRef
            || method.ReturnType != typeof(Task)
            && method.ReturnType != typeof(ValueTask))
            throw InvalidShape(sessionType, method);

        return (ZLinkSessionPacketHandlerDescriptor)Activator.CreateInstance(
            typeof(ZLinkAttributedSessionPacketHandlerDescriptor<>).MakeGenericType(messageType),
            sessionType,
            ZLinkMessageNameResolver.ResolveFromType(messageType),
            ZLinkHandlerMethodInvokerFactory.Create(method),
            passDispatch,
            passCancellationToken)!;
    }

    private static ZLinkConfigurationException InvalidShape(Type sessionType, MethodInfo method)
    {
        return new ZLinkConfigurationException(
            $"Stream packet method '{sessionType.FullName}.{method.Name}' must accept payload, optional ZLinkSessionDispatchContext, optional CancellationToken and return Task or ValueTask.");
    }

    public static ZLinkSessionPacketHandlerDescriptor Create(
        Type handlerType,
        Type sessionContextType,
        string? packetName)
    {
        foreach (var (definition, arguments) in ZLinkHandlerContractInspector.EnumerateGenericInterfaces(handlerType))
        {
            if (definition != typeof(IZLinkSessionPacketHandler<,>)) continue;

            var handlerContextType = arguments[0];
            if (!handlerContextType.IsAssignableFrom(sessionContextType))
                throw new ZLinkConfigurationException(
                    $"Session packet handler '{handlerType.FullName}' targets context '{handlerContextType.FullName}', but the session context is '{sessionContextType.FullName}'.");

            var messageType = arguments[1];
            return Create(handlerType, handlerContextType, messageType, packetName);
        }

        throw new ZLinkConfigurationException(
            $"Session packet handler '{handlerType.FullName}' must implement IZLinkSessionPacketHandler<TSessionContext, TMessage>.");
    }

    public static ZLinkSessionPacketHandlerDescriptor Create(
        Type handlerType,
        Type handlerContextType,
        Type messageType,
        string? packetName)
    {
        return (ZLinkSessionPacketHandlerDescriptor)Activator.CreateInstance(
            typeof(ZLinkSessionPacketHandlerDescriptor<,,>).MakeGenericType(
                handlerContextType,
                messageType,
                handlerType),
            packetName ?? ZLinkMessageNameResolver.ResolveFromType(messageType))!;
    }
}
