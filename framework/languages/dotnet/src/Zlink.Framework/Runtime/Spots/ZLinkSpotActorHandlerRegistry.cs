namespace Zlink.Framework.Runtime.Spots;

internal enum ZLinkSpotActorHandlerSurface
{
    EntrySpot,
    UserSpot
}

internal sealed class ZLinkSpotActorPacketDescriptor
{
    public required Type HandlerType { get; init; }

    public Type? SpotType { get; init; }

    public required Type ActorType { get; init; }

    public required Type MessageType { get; init; }

    public Type? ReplyType { get; init; }

    public required ZLinkMessageKind Kind { get; init; }

    public required ZLinkHandlerMethodInvoker Invoker { get; init; }

    public required string MessageName { get; init; }

    public required ZLinkSpotActorHandlerSurface Surface { get; init; }
}

internal sealed class ZLinkSpotActorLifecycleDescriptor
{
    public required Type HandlerType { get; init; }

    public Type? SpotType { get; init; }

    public required Type ActorType { get; init; }

    public required ZLinkHandlerMethodInvoker Invoker { get; init; }

    public required ZLinkSpotActorHandlerSurface Surface { get; init; }

    public bool PassSpotArgument { get; init; } = true;

    public bool PassRequestArgument { get; init; }
}

internal sealed class ZLinkSpotActorInferredHandlerDescriptor
{
    public ZLinkSpotActorPacketDescriptor? Packet { get; init; }

    public ZLinkSpotActorLifecycleDescriptor? Created { get; init; }

    public ZLinkSpotActorLifecycleDescriptor? Joined { get; init; }

    public ZLinkSpotActorLifecycleDescriptor? Left { get; init; }

    public ZLinkSpotActorLifecycleDescriptor? Disconnected { get; init; }
}

internal sealed class ZLinkSpotActorHandlerRegistry
{
    private readonly Dictionary<Type, ZLinkSpotActorLifecycleDescriptor> _created = [];
    private readonly Dictionary<Type, ZLinkSpotActorLifecycleDescriptor> _disconnected = [];
    private readonly Type? _expectedSpotType;
    private readonly Dictionary<Type, ZLinkSpotActorLifecycleDescriptor> _joined = [];
    private readonly Dictionary<Type, ZLinkSpotActorLifecycleDescriptor> _left = [];

    private readonly Dictionary<(ZLinkMessageKind Kind, Type ActorType, string Name), ZLinkSpotActorPacketDescriptor>
        _packets = [];

    private readonly ZLinkSpotActorHandlerSurface _surface;
    private bool _bound;

    public ZLinkSpotActorHandlerRegistry(
        ZLinkSpotActorHandlerSurface surface,
        Type? expectedSpotType = null)
    {
        _surface = surface;
        _expectedSpotType = expectedSpotType;
    }

    public void AddPacket(
        Type handlerType,
        Type actorType,
        string? packetName)
    {
        EnsureNotBound();
        var descriptor = ZLinkSpotActorHandlerDescriptorFactory.CreatePacket(
            _surface,
            _expectedSpotType,
            handlerType,
            actorType,
            packetName);

        AddPacketDescriptor(descriptor);
    }

    public void AddHandler(Type handlerType, string? packetName)
    {
        EnsureNotBound();
        var descriptor = ZLinkSpotActorHandlerDescriptorFactory.CreateInferred(
            _surface,
            _expectedSpotType,
            handlerType,
            packetName);

        if (descriptor.Packet is { } packet) AddPacketDescriptor(packet);
    }

    private void AddPacketDescriptor(ZLinkSpotActorPacketDescriptor descriptor)
    {
        var key = (descriptor.Kind, descriptor.ActorType, descriptor.MessageName);
        if (_packets.TryGetValue(key, out var existing)
            && existing.HandlerType == descriptor.HandlerType
            && existing.MessageType == descriptor.MessageType)
            return;

        if (existing is not null)
            throw new ZLinkConfigurationException(
                $"Actor packet '{descriptor.MessageName}' for '{descriptor.ActorType}' is already registered.");

        _packets.Add(key, descriptor);
    }

    public void Bind()
    {
        if (_expectedSpotType is not null)
            foreach (var descriptor in ZLinkSpotActorAttributedDescriptorFactory.CreateSpotLifecycleDescriptors(
                         _surface,
                         _expectedSpotType))
            {
                if (descriptor.Created is { } created) AddLifecycleDescriptor(_created, created);

                if (descriptor.Joined is { } joined) AddLifecycleDescriptor(_joined, joined);

                if (descriptor.Left is { } left) AddLifecycleDescriptor(_left, left);

                if (descriptor.Disconnected is { } disconnected) AddLifecycleDescriptor(_disconnected, disconnected);
            }

        _bound = true;
    }

    public bool TryResolve(
        Type actorType,
        ZlinkStreamHeader header,
        out ZLinkSpotActorPacketDescriptor? descriptor)
    {
        if (!ZLinkStreamActorPacketKind.TryMap(header.Kind, out var kind))
        {
            descriptor = null;
            return false;
        }

        if (_packets.TryGetValue((kind, actorType, header.Name), out descriptor)) return true;

        foreach (var candidate in _packets)
            if (candidate.Key.Kind == kind
                && candidate.Key.Name == header.Name
                && candidate.Key.ActorType.IsAssignableFrom(actorType))
            {
                descriptor = candidate.Value;
                return true;
            }

        descriptor = null;
        return false;
    }

    public bool TryResolveCreated(Type actorType, out ZLinkSpotActorLifecycleDescriptor? descriptor)
    {
        return TryResolveLifecycle(_created, actorType, out descriptor);
    }

    public bool TryResolveJoined(Type actorType, out ZLinkSpotActorLifecycleDescriptor? descriptor)
    {
        return TryResolveLifecycle(_joined, actorType, out descriptor);
    }

    public bool TryResolveLeft(Type actorType, out ZLinkSpotActorLifecycleDescriptor? descriptor)
    {
        return TryResolveLifecycle(_left, actorType, out descriptor);
    }

    public bool TryResolveDisconnected(Type actorType, out ZLinkSpotActorLifecycleDescriptor? descriptor)
    {
        return TryResolveLifecycle(_disconnected, actorType, out descriptor);
    }

    private static void AddLifecycleDescriptor(
        Dictionary<Type, ZLinkSpotActorLifecycleDescriptor> target,
        ZLinkSpotActorLifecycleDescriptor descriptor)
    {
        if (target.TryGetValue(descriptor.ActorType, out var existing)
            && existing.HandlerType == descriptor.HandlerType)
            return;

        if (existing is not null)
            throw new ZLinkConfigurationException(
                $"Actor lifecycle callback for '{descriptor.ActorType}' is already declared.");

        target.Add(descriptor.ActorType, descriptor);
    }

    private static bool TryResolveLifecycle(
        Dictionary<Type, ZLinkSpotActorLifecycleDescriptor> source,
        Type actorType,
        out ZLinkSpotActorLifecycleDescriptor? descriptor)
    {
        if (source.TryGetValue(actorType, out descriptor)) return true;

        foreach (var candidate in source)
            if (candidate.Key.IsAssignableFrom(actorType))
            {
                descriptor = candidate.Value;
                return true;
            }

        descriptor = null;
        return false;
    }

    private void EnsureNotBound()
    {
        if (_bound)
            throw new InvalidOperationException(
                "Actor handler registration is only allowed while Configure is running.");
    }
}
