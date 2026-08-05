namespace Zlink.Framework.Runtime.Spots;

internal sealed partial class ZLinkSpotActivation
{
    public void AddPacket<THandler>()
        where THandler : class
    {
        EnsureConfigurationOpen();
        _packets.Add(typeof(THandler));
    }

    public void AddSubscribe<THandler>(string channelName, string topic)
        where THandler : class
    {
        EnsureConfigurationOpen();
        _subscriptions.Add(channelName, topic, typeof(THandler));
    }

    public void AddHandler<THandler>()
        where THandler : class
    {
        EnsureConfigurationOpen();
        RequireActorHandlers().AddHandler(typeof(THandler), null);
    }

    public void AddHandler<THandler>(string packetName)
        where THandler : class
    {
        if (string.IsNullOrWhiteSpace(packetName))
            throw new InvalidOperationException("Actor packet name must not be empty.");

        EnsureConfigurationOpen();
        RequireActorHandlers().AddHandler(typeof(THandler), packetName);
    }

    public void AddActorPacket<THandler, TActor>()
        where THandler : class
        where TActor : IZLinkActor
    {
        AddActorPacketCore<THandler, TActor>(null);
    }

    public void AddActorPacket<THandler, TActor>(string packetName)
        where THandler : class
        where TActor : IZLinkActor
    {
        if (string.IsNullOrWhiteSpace(packetName))
            throw new InvalidOperationException("Actor packet name must not be empty.");

        AddActorPacketCore<THandler, TActor>(packetName);
    }

    public void AttachSpot(IZLinkSpot spot)
    {
        ArgumentNullException.ThrowIfNull(spot);
        if (_spot is not null) throw new InvalidOperationException("SPOT has already been attached to this context.");

        if (!ReferenceEquals(spot.Context, this))
            throw new InvalidOperationException(
                $"SPOT '{spot.GetType().FullName}' must expose the context provided by the runtime.");

        _spot = spot;
        _actorHandlers = new ZLinkSpotActorHandlerRegistry(
            ZLinkSpotActorHandlerSurface.UserSpot,
            spot.GetType());
        _handlerInvoker = new ZLinkSpotHandlerInvoker(
            _handlerInstances,
            spot,
            SpotNodeName,
            _runtime.Registration.Codecs,
            _runtime.Registration.StreamCompressionCodec,
            this,
            ResolveActorHandlerInstances);
    }

    public void AttachInstanceSpot(IZLinkInstanceSpot spot)
    {
        ArgumentNullException.ThrowIfNull(spot);
        if (_spot is not null)
            throw new InvalidOperationException("SPOT has already been attached to this context.");
        if (!ReferenceEquals(spot.Context, this))
            throw new InvalidOperationException(
                $"Instance Spot '{spot.GetType().FullName}' must expose the context provided by the runtime.");

        _spot = spot;
        _handlerInvoker = new ZLinkSpotHandlerInvoker(
            _handlerInstances,
            spot,
            SpotNodeName,
            _runtime.Registration.Codecs,
            _runtime.Registration.StreamCompressionCodec,
            this,
            ResolveActorHandlerInstances);
    }

    public async ValueTask BindDescriptorsAsync(CancellationToken cancellationToken)
    {
        _configurationOpen = false;

        _packets.Bind(Spot);
        if (Spot is IZLinkSpot)
        {
            await _subscriptions.BindAsync(
                    Spot,
                    NativeSpot,
                    DefaultRequestTimeout,
                    cancellationToken)
                .ConfigureAwait(false);
            _actorJoins.Bind(Spot);
            _actorHandlers?.Bind();
        }
    }

    internal async ValueTask ApplyScannedHandlerAsync(
        ZLinkScannedSpotHandler handler,
        CancellationToken cancellationToken)
    {
        if (handler.SpotType != Spot.GetType()) return;

        EnsureConfigurationOpen();
        switch (handler.Kind)
        {
            case ZLinkScannedSpotHandlerKind.Packet:
                _packets.Add(handler);
                return;
            case ZLinkScannedSpotHandlerKind.Subscription:
                if (Spot is IZLinkInstanceSpot)
                    throw new ZLinkConfigurationException(
                        $"Instance Spot '{Spot.GetType()}' cannot register subscription handlers.");
                if (handler.SpotNodeName is not null
                    && !string.Equals(handler.SpotNodeName, SpotNodeName, StringComparison.Ordinal)) return;
                var topic = handler.Topic
                            ?? throw new InvalidOperationException("Scanned SPOT subscription requires a topic.");
                var channelName = handler.ChannelName
                                  ?? throw new InvalidOperationException(
                                      "Scanned SPOT subscription requires a channel name.");
                if (handler.Method is { } subscriptionMethod)
                    _subscriptions.Add(channelName, topic, handler.HandlerType, subscriptionMethod);
                else
                    _subscriptions.Add(channelName, topic, handler.HandlerType);
                return;
            case ZLinkScannedSpotHandlerKind.ActorSend:
            case ZLinkScannedSpotHandlerKind.ActorRequest:
                if (Spot is IZLinkInstanceSpot)
                    throw new ZLinkConfigurationException(
                        $"Instance Spot '{Spot.GetType()}' cannot register Actor handlers.");
                RequireActorHandlers().AddPacket(
                    handler.HandlerType,
                    handler.ActorType ??
                    throw new InvalidOperationException("Scanned SPOT actor handler requires an actor type."),
                    handler.PacketName);
                return;
            case ZLinkScannedSpotHandlerKind.Timer:
                _ = await _timers.AddAsync(
                    handler.TimerName ?? throw new InvalidOperationException("Scanned SPOT timer requires a name."),
                    handler.TimerPeriod,
                    null,
                    handler.HandlerType,
                    Spot.GetType(),
                    StopToken,
                    DispatchTimerAsync,
                    PublishTimerFailureAsync,
                    cancellationToken).ConfigureAwait(false);
                return;
            default:
                throw new InvalidOperationException($"Unsupported scanned SPOT handler kind '{handler.Kind}'.");
        }
    }

    private void AddActorPacketCore<THandler, TActor>(string? packetName)
        where THandler : class
        where TActor : IZLinkActor
    {
        EnsureConfigurationOpen();
        RequireActorHandlers().AddPacket(typeof(THandler), typeof(TActor), packetName);
    }

    private ZLinkSpotActorHandlerRegistry RequireActorHandlers()
    {
        return _actorHandlers
               ?? throw new InvalidOperationException("SPOT actor registry is not initialized.");
    }

    private ZLinkScopedHandlerInstanceOwner ResolveActorHandlerInstances(
        IZLinkActor actor)
    {
        var state = _runtime.GetOrCreateActorState(actor.Context.ActorId);
        return state.HandlerInstances;
    }
}
