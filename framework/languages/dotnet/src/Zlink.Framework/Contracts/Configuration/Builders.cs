namespace Zlink.Framework.Contracts.Configuration;

// Build-time and runtime configuration use the same option contracts. Runtime
// changes are accepted only for properties whose public contract allows them.
public interface IZLinkStreamCompressionBuilder
{
    IZLinkStreamCompressionBuilder UseDefault();

    IZLinkStreamCompressionBuilder UseLz4();

    IZLinkStreamCompressionBuilder Use(IZlinkStreamCompressionCodec codec);

    IZLinkStreamCompressionBuilder Disable();
}

public interface IZLinkStreamNodeBuilder
{
    IZLinkStreamNodeBuilder Bind(string endpoint);

    IZLinkStreamNodeBuilder Bind(int port = 0);

    IZLinkStreamNodeBuilder SetBindHost(string bindHost);

    IZLinkStreamNodeBuilder SetAdvertiseHost(string advertiseHost);

    IZLinkSocketConfig ConfigureSocket();

    IZLinkStreamNodeBuilder EnableActorDispatch();

    IZLinkStreamNodeBuilder SetTlsServer(
        string certificatePath,
        string keyPath,
        bool requireClientCertificate = false);

    IZLinkStreamNodeBuilder AddSession<TSession>()
        where TSession : class, IZLinkSession;
}

public interface IZLinkFanoutChannelBuilder
{
    IZLinkFanoutChannelBuilder EnablePublisher(string endpoint);

    IZLinkFanoutChannelBuilder EnablePublisher(int port = 0);

    IZLinkFanoutChannelBuilder SetBindHost(string bindHost);

    IZLinkFanoutChannelBuilder SetAdvertiseHost(string advertiseHost);

    IZLinkFanoutChannelBuilder SetRoutingId(RoutingId publisherRoutingId);

    IZLinkFanoutChannelBuilder SetRoutingIdPrefix(string prefix);

    IZLinkFanoutChannelBuilder EnableSubscriber();

    IZLinkFanoutChannelBuilder Connect(string endpoint);

    IZLinkEndpointConnections SubscriberConnections { get; }

    IZLinkFanoutChannelBuilder AddHandler<THandler, TEvent>(string? packetName = null)
        where THandler : class, IZLinkFanoutHandler<TEvent>;
}

public interface IZLinkClientServerChannelRoleBuilder
{
    IZLinkClientServerChannelClientBuilder Client();

    IZLinkClientServerChannelServerBuilder Server();
}

public interface IZLinkClientServerChannelClientBuilder
{
    IZLinkClientServerChannelClientBuilder Connect(string endpoint);
}

public interface IZLinkClientServerChannelServerBuilder
{
    IZLinkClientServerChannelServerBuilder Listen(int port = 0);

    IZLinkClientServerChannelServerBuilder SetBindHost(string bindHost);

    IZLinkClientServerChannelServerBuilder SetAdvertiseHost(string advertiseHost);

    IZLinkClientServerChannelServerBuilder SetWeight(int weight);

    IZLinkClientServerChannelServerBuilder AddHandlerGroup(string groupName);

    IZLinkClientServerChannelServerBuilder AddSendHandler<THandler, TMessage>(
        string? packetName = null)
        where THandler : class, IZLinkSendHandler<TMessage>;

    IZLinkClientServerChannelServerBuilder AddRequestHandler<THandler, TRequest, TReply>(
        string? packetName = null)
        where THandler : class, IZLinkRequestHandler<TRequest, TReply>;
}

// The unified MeshNode registration surface and supporting types live in
// MeshNodeBuilders.cs.

public interface IZLinkFrameworkOptions
{
    TimeSpan DefaultRequestTimeout { get; set; }

    /// <summary>
    ///     Gets or sets the default timeout used while submitting a socket send.
    /// </summary>
    TimeSpan DefaultSocketSendTimeout { get; set; }

    long ApplicationVersion { get; set; }

    string? MaintenanceWave { get; set; }

    IZLinkCodecRegistryBuilder Codecs { get; }

    IZLinkWorkerOptions Worker { get; }

    void AddHandlersFromAssemblyOf<TMarker>();

    void AddHandlersFromAssemblyOf(Type markerType);

    void AddHandlersFromAssembly(System.Reflection.Assembly assembly);

    void DisableImplicitHandlerAutoRegistration();

    IZLinkMetadataPolicyBuilder ConfigureMetadata();

    IZLinkFanoutChannelBuilder AddFanoutChannel(string channelName);

    IZLinkClientServerChannelRoleBuilder AddClientServerChannel(string channelName);

    /// <summary>
    /// Registers the opaque location store used by Framework records that
    /// require atomic conditional publication. Provider-specific watch,
    /// change-stamp, schema, and key-layout capabilities are not part of this
    /// public contract.
    /// </summary>
    void AddLocationStore(LocationProvider.IZLinkLocationStore store);

    void AddRelocationStore(LocationProvider.IZLinkRelocationStore store);

    Locations.ZLinkLocationOptions ConfigureLocations();

    IZLinkInboundDispatchOptions ConfigureInboundDispatch();

    IZLinkNetworkOptions ConfigureNetwork();

    void UseFilter<TFilter>()
        where TFilter : class, IZLinkHandlerFilter;

    IZLinkDispatchOptions ConfigureDispatch();

    IZLinkStreamCompressionBuilder ConfigureStreamCompression();

    IZLinkStreamNodeBuilder AddStreamNode(string streamNodeName);

    // Registers one process-local MeshNode under meshName (spec 05-route-mesh §2).
    // Registering the same meshName twice fails host startup. The MeshNode owns its
    // ROUTER endpoint, logical channel memberships, RID-direct route handlers and
    // Spot/Actor registry.
    IZLinkMeshNodeBuilder AddRouteMesh(string meshName);
}

public interface IZLinkNetworkOptions
{
    string BindHost { get; set; }

    string? AdvertiseHost { get; set; }
}

public interface IZLinkMetadataPolicyBuilder
{
    IZLinkMetadataPolicyBuilder AllowSessionToActor(string key);

    IZLinkMetadataPolicyBuilder AllowActorToSession(string key);
}
