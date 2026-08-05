using Microsoft.Extensions.DependencyInjection;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotStartupValidator
{
    public static async ValueTask ValidateAsync(
        IServiceProvider services,
        ZLinkFrameworkRegistration registration)
    {
        foreach (var spotNode in registration.SpotNodes.Values)
        foreach (var spotType in spotNode.SpotFactories)
        {
            await using var scope = services.CreateAsyncScope();
            var context = new StartupConfigurationContext(
                spotType,
                spotNode.SpotNodeName,
                spotNode.SpotMeshChannelName ?? spotNode.SpotNodeName);
            var spot = (IZLinkSpot)ActivatorUtilities.CreateInstance(
                scope.ServiceProvider,
                spotType,
                context);
            if (!ReferenceEquals(spot.Context, context))
                throw new ZLinkConfigurationException(
                    $"SPOT '{spotType}' must expose the context provided by the runtime.");

            foreach (var handler in registration.ScannedHandlerCatalog.SpotHandlers)
                context.AddScannedHandler(spotType, handler);

            spot.Configure();
            context.Validate(spot);
        }
    }

    private sealed class StartupConfigurationContext :
        IZLinkSpotContext,
        IZLinkSpotHandlerRegistrySink
    {
        private readonly ZLinkSpotActorHandlerRegistry _actorHandlers;
        private readonly ZLinkSpotPacketRegistry _packets = new();
        private readonly string _spotNodeName;
        private readonly ZLinkSpotSubscriptionRegistry _subscriptions = new();

        public StartupConfigurationContext(
            Type spotType,
            string spotNodeName,
            string meshName)
        {
            _actorHandlers = new ZLinkSpotActorHandlerRegistry(
                ZLinkSpotActorHandlerSurface.UserSpot,
                spotType);
            _spotNodeName = spotNodeName;
            MeshName = meshName;
            Handlers = new ZLinkSpotHandlerRegistrySurface(this);
        }

        public string MeshName { get; }

        public IZLinkSpotHandlerRegistry Handlers { get; }

        public string SpotId => string.Empty;

        public ulong ObjectGeneration => default;

        public RoutingId NodeRid => default;

        public IZLinkSpotOutbound Outbound => throw ConfigurationOnly();

        public ValueTask<IZLinkTimer> AddTimer<THandler>(
            string name,
            TimeSpan period,
            ZLinkTimerOptions? options = null,
            CancellationToken cancellationToken = default)
            where THandler : class => throw ConfigurationOnly();

        public IZLinkWorkerCall<TResult> RunCpuWorker<TResult>(
            Func<CancellationToken, TResult> work) => throw ConfigurationOnly();

        public IZLinkWorkerCall<TResult> RunIoWorker<TResult>(
            Func<CancellationToken, ValueTask<TResult>> work) => throw ConfigurationOnly();

        public IZLinkSpotRelocationReadyCall RelocationReady() =>
            throw ConfigurationOnly();

        public ValueTask LeaveActorAsync(
            IZLinkActor actor,
            CancellationToken cancellationToken = default) => throw ConfigurationOnly();

        public ValueTask<bool> CloseAsync(
            CancellationToken cancellationToken = default) => throw ConfigurationOnly();

        public void AddPacket<THandler>() where THandler : class =>
            _packets.Add(typeof(THandler));

        public void AddSubscribe<THandler>(string channelName, string topic) where THandler : class
        {
            _subscriptions.Add(channelName, topic, typeof(THandler));
        }

        public void AddHandler<THandler>() where THandler : class
        {
            _actorHandlers.AddHandler(typeof(THandler), null);
        }

        public void AddHandler<THandler>(string packetName) where THandler : class
        {
            if (string.IsNullOrWhiteSpace(packetName))
                throw new ZLinkConfigurationException("Actor packet name must not be empty.");
            _actorHandlers.AddHandler(typeof(THandler), packetName);
        }

        public void AddActorPacket<THandler, TActor>()
            where THandler : class
            where TActor : IZLinkActor
        {
            _actorHandlers.AddPacket(typeof(THandler), typeof(TActor), null);
        }

        public void AddActorPacket<THandler, TActor>(string packetName)
            where THandler : class
            where TActor : IZLinkActor
        {
            if (string.IsNullOrWhiteSpace(packetName))
                throw new ZLinkConfigurationException("Actor packet name must not be empty.");
            _actorHandlers.AddPacket(typeof(THandler), typeof(TActor), packetName);
        }

        public void AddScannedHandler(
            Type spotType,
            ZLinkScannedSpotHandler handler)
        {
            if (handler.SpotType != spotType) return;

            switch (handler.Kind)
            {
                case ZLinkScannedSpotHandlerKind.Packet:
                    _packets.Add(handler);
                    break;
                case ZLinkScannedSpotHandlerKind.Subscription:
                    if (handler.SpotNodeName is not null
                        && !string.Equals(handler.SpotNodeName, _spotNodeName, StringComparison.Ordinal)) break;
                    var topic = handler.Topic
                                ?? throw new ZLinkConfigurationException(
                                    "Scanned SPOT subscription requires a topic.");
                    var channelName = handler.ChannelName
                                      ?? throw new ZLinkConfigurationException(
                                          "Scanned SPOT subscription requires a channel name.");
                    if (handler.Method is { } subscriptionMethod)
                        _subscriptions.Add(channelName, topic, handler.HandlerType, subscriptionMethod);
                    else
                        _subscriptions.Add(channelName, topic, handler.HandlerType);
                    break;
                case ZLinkScannedSpotHandlerKind.ActorSend:
                case ZLinkScannedSpotHandlerKind.ActorRequest:
                    _actorHandlers.AddPacket(
                        handler.HandlerType,
                        handler.ActorType
                        ?? throw new ZLinkConfigurationException(
                            "Scanned SPOT actor handler requires an actor type."),
                        handler.PacketName);
                    break;
                case ZLinkScannedSpotHandlerKind.Timer:
                    ZLinkSpotTimerRegistry.ValidateRegistration(
                        handler.TimerName ?? string.Empty,
                        handler.TimerPeriod,
                        null);
                    break;
                default:
                    throw new ZLinkConfigurationException(
                        $"Unsupported scanned SPOT handler kind '{handler.Kind}'.");
            }
        }

        public void Validate(IZLinkSpot spot)
        {
            _packets.Bind(spot);
            _subscriptions.Validate(spot);
            _actorHandlers.Bind();
        }

        private static ZLinkConfigurationException ConfigurationOnly() => new(
            "SPOT lifecycle operations are not available while startup configuration is being validated.");
    }
}
