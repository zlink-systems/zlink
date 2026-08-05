namespace Zlink.Framework.Runtime.Configuration;

internal static partial class ZLinkFrameworkRegistrationValidator
{
    private static void ValidateChannel(
        ZLinkChannelRegistration channel,
        bool autoConnectConfigured,
        ZLinkHandlerExposureCatalog handlerExposure)
    {
        ValidateChannelShape(channel);

        if (channel.Client is not null)
        {
            // A process-local Server is a complete peer source for the Client.
            // The runtime still uses DEALER -> ROUTER transport; it only obtains
            // the bound endpoint directly instead of through a Location Store.
            var hasLocalServer = channel.HasClientServerServer;
            ZLinkPeerAcquisitionPolicy.RequirePeerSource(
                $"ClientServer channel '{channel.ChannelName}' client",
                autoConnectConfigured || hasLocalServer,
                channel.Client.ManualConnections);
            channel.Client.AcquisitionMode = ZLinkPeerAcquisitionPolicy.Resolve(
                autoConnectConfigured || hasLocalServer,
                channel.Client.ManualConnections);
            channel.Client.ManualConnections.Freeze(channel.Client.AcquisitionMode);
        }

        if (channel.Publisher is { } publisher)
        {
            if (string.IsNullOrWhiteSpace(publisher.BindEndpoint)
                && publisher.ListenPort is null)
                throw new ZLinkConfigurationException(
                    $"fanout channel '{channel.ChannelName}' publisher must define a listener.");
            if (publisher.FixedRoutingId.Size > 0
                && publisher.RoutingIdPrefix is not null)
                throw new ZLinkConfigurationException(
                    $"fanout channel '{channel.ChannelName}' publisher cannot combine a fixed routing id and a routing-id prefix.");
            if (autoConnectConfigured && publisher.FixedRoutingId.Size > 0)
                throw new ZLinkConfigurationException(
                    $"fanout channel '{channel.ChannelName}' automatic publisher cannot use a fixed routing id.");
            if (!autoConnectConfigured && publisher.RoutingIdPrefix is not null)
                throw new ZLinkConfigurationException(
                    $"fanout channel '{channel.ChannelName}' manual publisher cannot allocate a routing id without a location store.");
        }

        if (channel.Subscriber is not null)
        {
            var automatic = channel.Subscriber.AutomaticDiscoveryEnabled;
            if (automatic && channel.Subscriber.ManualConnections.Count > 0)
                throw new ZLinkConfigurationException(
                    $"fanout channel '{channel.ChannelName}' cannot combine automatic and manual subscriber endpoints.");
            if (automatic && !autoConnectConfigured)
                throw new ZLinkConfigurationException(
                    $"fanout channel '{channel.ChannelName}' automatic subscriber requires a location store.");
            if (!automatic && channel.Subscriber.ManualConnections.Count == 0)
                throw new ZLinkConfigurationException(
                    $"fanout channel '{channel.ChannelName}' manual subscriber requires at least one endpoint.");
            channel.Subscriber.AcquisitionMode = automatic
                ? ZLinkPeerAcquisitionMode.AutoConnect
                : ZLinkPeerAcquisitionMode.Manual;
            channel.Subscriber.ManualConnections.Freeze(channel.Subscriber.AcquisitionMode);
        }

        var exposedKinds = handlerExposure.ValidateChannel(channel);
        if (channel.AutoConnectType == ZLinkLocationAutoConnectType.ClientServer)
            ValidateClientServerHandlerExposure(channel, exposedKinds);
        else
            ValidateFanoutHandlerExposure(channel, exposedKinds);
    }

    private static void ValidateClientServerHandlerExposure(
        ZLinkChannelRegistration channel,
        IReadOnlySet<ZLinkMessageKind> exposedKinds)
    {
        var hasHandlers = exposedKinds.Contains(ZLinkMessageKind.Command)
                          || exposedKinds.Contains(ZLinkMessageKind.Request);
        if (channel.HasClientServerClient
            && !channel.HasClientServerServer
            && hasHandlers)
            throw new ZLinkConfigurationException(
                $"ClientServer channel '{channel.ChannelName}' client cannot expose handlers.");
        if (channel.HasClientServerServer && !hasHandlers)
            throw new ZLinkConfigurationException(
                $"ClientServer channel '{channel.ChannelName}' server must expose a send or request handler.");
        if (channel.PublishHandlers.Count > 0)
            throw new ZLinkConfigurationException(
                $"ClientServer channel '{channel.ChannelName}' cannot register publish handlers.");
    }

    private static void ValidateFanoutHandlerExposure(
        ZLinkChannelRegistration channel,
        IReadOnlySet<ZLinkMessageKind> exposedKinds)
    {
        var hasHandlerExposure = exposedKinds.Contains(ZLinkMessageKind.Publish);

        if (hasHandlerExposure && channel.Subscriber is null)
            throw new ZLinkConfigurationException(
                $"fanout channel '{channel.ChannelName}' exposes publish handlers but does not enable subscriber capability.");

        if (channel.Subscriber is not null && !hasHandlerExposure)
            throw new ZLinkConfigurationException(
                $"fanout channel '{channel.ChannelName}' subscriber must map a publish handler group or register a typed publish handler.");

        if (channel.SendHandlers.Count > 0 || channel.RequestHandlers.Count > 0)
            throw new ZLinkConfigurationException(
                $"fanout channel '{channel.ChannelName}' cannot register send or request handlers.");
    }

    private static void ValidateChannelShape(ZLinkChannelRegistration channel)
    {
        if (channel.AutoConnectType == ZLinkLocationAutoConnectType.ClientServer)
        {
            var validClient = channel.HasClientServerClient == (channel.Client is not null);
            var validServer = channel.HasClientServerServer == (channel.Server is not null);
            if ((!channel.HasClientServerClient && !channel.HasClientServerServer)
                || !validClient
                || !validServer)
                throw new ZLinkConfigurationException(
                    $"ClientServer channel '{channel.ChannelName}' must register Client or Server at least once, and each role at most once.");
            if (channel.Publisher is not null || channel.Subscriber is not null)
                throw new ZLinkConfigurationException(
                    $"ClientServer channel '{channel.ChannelName}' cannot enable fanout capabilities.");
            return;
        }

        if (channel.AutoConnectType != ZLinkLocationAutoConnectType.Fanout)
            throw new ZLinkConfigurationException(
                $"channel '{channel.ChannelName}' must declare fanout topology.");
        if (channel.Publisher is null && channel.Subscriber is null)
            throw new ZLinkConfigurationException(
                $"fanout channel '{channel.ChannelName}' must enable publisher or subscriber capabilities.");
    }
}
