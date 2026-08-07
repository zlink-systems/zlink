using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelBundleFactory(
    ZLinkFrameworkRegistration registration)
{
    public async ValueTask<ZLinkChannelRuntimeBundle> CreateClientServerClientBundleAsync(
        ZLinkFrameworkComponentState state,
        string channelName,
        ZLinkChannelRegistration channel)
    {
        IDealerSocket? dealer = null;
        ZLinkChannelRuntimeBundle? bundle = null;
        try
        {
            dealer = state.Context.CreateDealerSocket();
            ApplySocketConfig(dealer.Options, channel.Client!.SocketConfig);
            dealer.Options.Probe = true;
            bundle = new ZLinkChannelRuntimeBundle(
                dealer,
                dealer.Connect,
                dealer.Disconnect,
                new ZLinkAsyncSubmitter(
                    dealer.OnSendReady,
                    channel.Client.SocketConfig.SendTimeout
                    ?? registration.DefaultSocketSendTimeout,
                    state.StopTokenSource.Token,
                    ZLinkAsyncSubmitter.ResolvePendingCapacity()),
                socketRole: "client");

            bundle.OwnManualConnectionAttachment(channel.Client.ManualConnections.Attach(
                bundle.ConnectManual,
                bundle.DisconnectManual));
            return bundle;
        }
        catch (Exception initializationFailure)
        {
            await ThrowAfterCleanupAsync(initializationFailure, bundle, dealer).ConfigureAwait(false);
            throw new InvalidOperationException("Unreachable after startup cleanup failure propagation.");
        }
    }

    public async ValueTask<ZLinkChannelRuntimeBundle> CreateClientServerServerBundleAsync(
        ZLinkFrameworkComponentState state,
        string channelName,
        ZLinkChannelRegistration channel)
    {
        IRouterSocket? router = null;
        ZLinkChannelRuntimeBundle? bundle = null;
        try
        {
            router = state.Context.CreateRouterSocket();
            var serverRid = channel.Server!.ServerRid;
            router.SetRoutingId(serverRid);
            ApplySocketConfig(router.Options, channel.Server!.SocketConfig);
            router.Options.Mandatory = true;
            router.Options.Handover = true;
            router.Bind(ZLinkNetworkEndpointResolver.Bind(
                explicitEndpoint: null,
                channel.Server.ListenPort,
                channel.Server.BindHost,
                registration.NetworkOptions));
            var actualEndpoint = router.Options.LastEndpoint;
            var identity = new ZLinkClientServerServerIdentity(
                channelName,
                serverRid,
                CreateLifecycleGeneration(),
                ZLinkTransportSecurityIdentity.Plaintext,
                channel.Server.SocketConfig.Weight,
                ZLinkClientServerControlProtocol.NormalizeMaximumMessageBytes(
                    channel.Server.SocketConfig.MaxMessageSize),
                ZLinkNetworkEndpointResolver.Advertise(
                    actualEndpoint,
                    channel.Server.AdvertiseHost,
                    channel.Server.BindHost,
                    registration.NetworkOptions));
            bundle = new ZLinkChannelRuntimeBundle(
                router,
                localRid: serverRid,
                socketRole: "server",
                clientServerServer: identity);
            return bundle;
        }
        catch (Exception initializationFailure)
        {
            await ThrowAfterCleanupAsync(initializationFailure, bundle, router).ConfigureAwait(false);
            throw new InvalidOperationException("Unreachable after startup cleanup failure propagation.");
        }
    }

    public async ValueTask<ZLinkChannelRuntimeBundle> CreateSubscriberBundleAsync(
        ZLinkFrameworkComponentState state,
        string channelName,
        ZLinkChannelRegistration channel)
    {
        ISubSocket? subscriber = null;
        ZLinkChannelRuntimeBundle? bundle = null;
        try
        {
            subscriber = state.Context.CreateSubscriberSocket();
            ApplySocketConfig(subscriber.Options, channel.Subscriber!.SocketConfig);
            RoutingId localRid = default;
            if (channel.RoutingId.Size > 0)
            {
                localRid = ZLinkRoutingIdPolicy.Derive(channel.RoutingId, "sub");
                // SUB has no addressable routing-id socket option. Keep the allocated identity
                // in the framework bundle for tracking and diagnostics; it is not a publish
                // destination and therefore is not written to the native socket.
            }

            subscriber.SetSubscription(string.Empty);
            bundle = new ZLinkChannelRuntimeBundle(
                subscriber,
                subscriber.Connect,
                subscriber.Disconnect,
                localRid: localRid,
                socketRole: "sub");

            if (channel.Subscriber.AcquisitionMode == ZLinkPeerAcquisitionMode.Manual)
                bundle.OwnManualConnectionAttachment(channel.Subscriber.ManualConnections.Attach(
                    bundle.ConnectManual,
                    bundle.DisconnectManual));

            return bundle;
        }
        catch (Exception initializationFailure)
        {
            await ThrowAfterCleanupAsync(initializationFailure, bundle, subscriber).ConfigureAwait(false);
            throw new InvalidOperationException("Unreachable after startup cleanup failure propagation.");
        }
    }

    public async ValueTask<ZLinkChannelRuntimeBundle> CreatePublisherBundleAsync(
        ZLinkFrameworkComponentState state,
        string channelName,
        ZLinkChannelRegistration channel)
    {
        IPubSocket? publisher = null;
        ZLinkChannelRuntimeBundle? bundle = null;
        try
        {
            publisher = state.Context.CreatePublisherSocket();
            ApplySocketConfig(publisher.Options, channel.Publisher!.SocketConfig);
            var localRid = ResolvePublisherRid(channelName, channel.Publisher);
            publisher.Bind(ResolvePublisherBindEndpoint(channel.Publisher));
            var publisherIdentity = new ZLinkFanoutPublisherIdentity(
                channelName,
                localRid,
                CreateLifecycleGeneration(),
                ZLinkNetworkEndpointResolver.Advertise(
                    publisher.Options.LastEndpoint,
                    channel.Publisher.AdvertiseHost,
                    channel.Publisher.BindHost,
                    registration.NetworkOptions));
            bundle = new ZLinkChannelRuntimeBundle(
                socket: publisher,
                submitter: new ZLinkAsyncSubmitter(
                    publisher.OnSendReady,
                    channel.Publisher.SocketConfig.SendTimeout ?? registration.DefaultSocketSendTimeout,
                    state.StopTokenSource.Token),
                localRid: localRid,
                socketRole: "pub",
                fanoutPublisher: publisherIdentity);

            return bundle;
        }
        catch (Exception initializationFailure)
        {
            await ThrowAfterCleanupAsync(initializationFailure, bundle, publisher).ConfigureAwait(false);
            throw new InvalidOperationException("Unreachable after startup cleanup failure propagation.");
        }
    }

    private static RoutingId ResolvePublisherRid(
        string channelName,
        ZLinkChannelPublisherCapabilityRegistration publisher)
    {
        if (publisher.FixedRoutingId.Size > 0)
            return publisher.FixedRoutingId;
        return ZLinkFanoutRoutingIdPolicy.Create(
            publisher.RoutingIdPrefix ?? channelName);
    }

    private string ResolvePublisherBindEndpoint(
        ZLinkChannelPublisherCapabilityRegistration publisher) =>
        ZLinkNetworkEndpointResolver.Bind(
            publisher.BindEndpoint,
            publisher.ListenPort,
            publisher.BindHost,
            registration.NetworkOptions);

    internal static void ApplySocketConfig(
        CommonSocketOptions socket,
        IZLinkSocketConfig config)
    {
        ZLinkBackendSocketOptionsMapper.Apply(socket, config);
    }

    private static ulong CreateLifecycleGeneration()
    {
        Span<byte> bytes = stackalloc byte[sizeof(ulong)];
        ulong value;
        do
        {
            System.Security.Cryptography.RandomNumberGenerator.Fill(bytes);
            value = System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(
                bytes);
        } while (value is 0 or > long.MaxValue);
        return value;
    }

    private static async ValueTask ThrowAfterCleanupAsync(
        Exception initializationFailure,
        IAsyncDisposable? composite,
        IAsyncDisposable? standalone)
    {
        var failures = new ZLinkFailureCollector(initializationFailure);
        if (composite is not null)
            await failures.CaptureAsync(composite.DisposeAsync).ConfigureAwait(false);
        else if (standalone is not null)
            await failures.CaptureAsync(standalone.DisposeAsync).ConfigureAwait(false);
        failures.ThrowIfAny();
    }

}
