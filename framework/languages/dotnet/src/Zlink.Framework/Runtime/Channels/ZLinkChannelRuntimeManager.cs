namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelRuntimeManager(
    IZLinkBackendAdapterFactory backendAdapterFactory,
    ZLinkFrameworkRegistration registration,
    ZLinkChannelReceiveLoop receiveLoop,
    ZLinkFanoutRuntimeService fanoutRuntime)
{
    private readonly ZLinkChannelBundleFactory _bundleFactory = new(backendAdapterFactory, registration);

    public ZLinkChannelRuntimeBundle GetPublisherBundle(
        ZLinkFrameworkComponentState state,
        string channelName)
    {
        lock (state.SyncRoot)
        {
            if (state.PublisherBundles.TryGetValue(channelName, out var existing)) return existing;
            if (!registration.Channels.TryGetValue(channelName, out var channel)
                || channel.Publisher is null)
                throw new ZLinkConfigurationException($"Channel publisher '{channelName}' is not registered.");

            throw new ZLinkConfigurationException($"Channel publisher '{channelName}' is not initialized.");
        }
    }

    public ZLinkChannelRuntimeBundle GetClientServerClientBundle(
        ZLinkFrameworkComponentState state,
        string channelName)
    {
        lock (state.SyncRoot)
        {
            if (state.ClientServerClientBundles.TryGetValue(channelName, out var bundle))
                return bundle;
            if (!registration.Channels.TryGetValue(channelName, out var channel)
                || !channel.HasClientServerClient)
                throw new ZLinkConfigurationException(
                    $"ClientServer client channel '{channelName}' is not registered.");
            throw new ZLinkConfigurationException(
                $"ClientServer client channel '{channelName}' is not initialized.");
        }
    }

    public ZLinkClientServerClientRuntime GetClientServerClientRuntime(
        ZLinkFrameworkComponentState state,
        string channelName)
    {
        lock (state.SyncRoot)
        {
            if (state.ClientServerClientRuntimes.TryGetValue(
                    channelName,
                    out var runtime))
                return runtime;
            throw new ZLinkConfigurationException(
                $"ClientServer client channel '{channelName}' is not initialized.");
        }
    }

    public async ValueTask InitializeInboundChannelsAsync(
        ZLinkFrameworkComponentState state,
        IZLinkChannelBackendAdapter adapter)
    {
        foreach (var entry in registration.Channels)
        {
            var channelName = entry.Key;
            var channel = entry.Value;

            if (channel.HasClientServerServer)
            {
                var bundle = await _bundleFactory.CreateClientServerServerBundleAsync(
                        state,
                        adapter,
                        channelName,
                        channel)
                    .ConfigureAwait(false);
                state.ClientServerServerBundles.Add(channelName, bundle);
                state.ListenerTasks.Add(state.TaskRunner.RunLongRunning(
                    $"client-server:{channelName}",
                    ct => new ValueTask(receiveLoop.RunClientServerLoopAsync(
                        channelName,
                        (IZLinkBackendRouterSocket)bundle.Socket,
                        bundle.ClientServerServer
                        ?? throw new InvalidOperationException(
                            "ClientServer server identity is not initialized."),
                        state.ErrorSink,
                        state.InboundDispatchBudget,
                        ct))));
            }

            if (channel.Subscriber is not null)
            {
                if (channel.Subscriber.AcquisitionMode
                    == ZLinkPeerAcquisitionMode.AutoConnect)
                {
                    state.AutomaticFanoutSubscriberRuntimes.Add(
                        channelName,
                        new ZLinkAutomaticFanoutSubscriberRuntime(
                            channelName,
                            backendAdapterFactory,
                            state.Context,
                            channel.Subscriber.SocketConfig,
                            receiveLoop,
                            fanoutRuntime,
                            state.ErrorSink,
                            state.StopTokenSource.Token,
                            inboundDispatchBudget: state.InboundDispatchBudget));
                    continue;
                }

                var bundle = await _bundleFactory.CreateSubscriberBundleAsync(state, adapter, channelName, channel)
                    .ConfigureAwait(false);
                state.SubscriberBundles.Add(channelName, bundle);
                state.ListenerTasks.Add(state.TaskRunner.RunLongRunning(
                    $"channel-subscriber:{channelName}",
                    ct => new ValueTask(receiveLoop.RunSubscriberLoopAsync(
                        channelName,
                        (IZLinkBackendSubscriberSocket)bundle.Socket,
                        state.ErrorSink,
                        state.InboundDispatchBudget,
                        ct))));
            }
        }
    }

    public async ValueTask InitializePublisherChannelsAsync(
        ZLinkFrameworkComponentState state,
        IZLinkChannelBackendAdapter adapter)
    {
        foreach (var entry in registration.Channels)
        {
            if (entry.Value.HasClientServerClient)
            {
                var channel = entry.Value;
                var runtime = new ZLinkClientServerClientRuntime(
                    entry.Key,
                    adapter,
                    backendAdapterFactory.CreateMonitoringAdapter(),
                    state.Context,
                    channel.Client!.SocketConfig,
                    channel.Client.SocketConfig.SendTimeout
                    ?? registration.DefaultSocketSendTimeout,
                    state.StopTokenSource.Token);
                state.ClientServerClientRuntimes.Add(entry.Key, runtime);
                runtime.OwnManualConnectionAttachment(channel.Client.ManualConnections.Attach(
                    runtime.AddManual,
                    runtime.RemoveManual));
                if (!registration.Locations.Enabled
                    && channel.HasClientServerServer
                    && state.ClientServerServerBundles.TryGetValue(
                        entry.Key,
                        out var localServer))
                {
                    runtime.AddLocal(
                        ((IZLinkBackendRouterSocket)localServer.Socket)
                        .GetLastEndpoint(),
                        localServer.ClientServerServer
                        ?? throw new InvalidOperationException(
                            "ClientServer server identity is not initialized."));
                }
            }

            if (entry.Value.Publisher is null) continue;

            var publisherBundle =
                await _bundleFactory.CreatePublisherBundleAsync(
                        state,
                        entry.Key,
                        entry.Value,
                        adapter)
                    .ConfigureAwait(false);
            state.PublisherBundles.Add(entry.Key, publisherBundle);
            state.ListenerTasks.Add(state.TaskRunner.Run(
                $"fanout-beacon:{entry.Key}",
                cancellationToken => RunFanoutBeaconLoopAsync(
                    (IZLinkBackendPublisherSocket)publisherBundle.Socket,
                    cancellationToken)));
        }
    }

    private static async ValueTask RunFanoutBeaconLoopAsync(
        IZLinkBackendPublisherSocket publisher,
        CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(
            ZLinkFanoutLivenessProtocol.BeaconInterval);
        while (await timer.WaitForNextTickAsync(cancellationToken)
                   .ConfigureAwait(false))
        {
            using var payload = Message.From(
                ZLinkFanoutLivenessProtocol.Payload);
            _ = publisher.Publish(
                ZLinkFanoutLivenessProtocol.Topic,
                payload,
                SendFlags.DontWait);
        }
    }

    public IZLinkBackendSocket GetMonitoringSocket(
        ZLinkFrameworkComponentState state,
        string sourceName)
    {
        var (channelName, capability) = ParseChannelCapabilitySource(sourceName);

        return capability switch
        {
            "subscriber" => state.SubscriberBundles.TryGetValue(channelName, out var subscriberBundle)
                ? subscriberBundle.Socket
                : throw new InvalidOperationException(
                    $"Socket monitoring source '{sourceName}' is not registered."),
            "publisher" => GetPublisherBundle(state, channelName).Socket,
            "client" => GetClientServerClientRuntime(
                state,
                channelName).GetMonitoringSocket(),
            "server" => state.ClientServerServerBundles.TryGetValue(channelName, out var serverBundle)
                ? serverBundle.Socket
                : throw new InvalidOperationException(
                    $"Socket monitoring source '{sourceName}' is not registered."),
            _ => throw new InvalidOperationException(
                $"Socket monitoring source '{sourceName}' is not registered.")
        };
    }

    private static (string ChannelName, string Capability) ParseChannelCapabilitySource(string sourceName)
    {
        ArgumentException.ThrowIfNullOrEmpty(sourceName);

        var separatorIndex = sourceName.LastIndexOf('.');
        if (separatorIndex <= 0 || separatorIndex == sourceName.Length - 1)
            throw new InvalidOperationException(
                $"Socket monitoring source '{sourceName}' must use '<channel>.<capability>'.");

        return (sourceName[..separatorIndex], sourceName[(separatorIndex + 1)..]);
    }
}
