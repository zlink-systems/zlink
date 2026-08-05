using System.Reflection;
using System.Collections.Frozen;

namespace Zlink.Framework.Runtime.Configuration;

internal sealed class ZLinkFrameworkRegistration
{
    private ZLinkScannedHandlerCatalog? _scannedHandlerCatalog;

    public TimeSpan DefaultRequestTimeout { get; set; } = TimeSpan.FromSeconds(30);

    public TimeSpan DefaultSocketSendTimeout { get; set; } = TimeSpan.FromMilliseconds(1000);

    public long ApplicationVersion { get; set; }

    public string? MaintenanceWave { get; set; }

    public ZLinkCodecRegistryBuilder Codecs { get; } = new();

    public IZlinkStreamCompressionCodec? StreamCompressionCodec { get; set; } =
        ZLinkStreamProtocolDefaults.CreateLz4CompressionCodec();

    public ZLinkMetadataPolicyRegistration MetadataPolicy { get; } = new();

    public ZLinkDispatchOptionsModel DispatchOptions { get; } = new();

    public ZLinkWorkerOptionsModel WorkerOptions { get; } = new();

    public ZLinkInboundDispatchOptionsModel InboundDispatchOptions { get; } = new();

    public ZLinkNetworkOptionsModel NetworkOptions { get; } = new();

    public List<Type> Filters { get; } = [];

    public ZLinkLocationRegistration Locations { get; } = new();

    public HashSet<Assembly> HandlerAssemblies { get; } = [];

    public bool ImplicitHandlerAutoRegistrationEnabled { get; set; } = true;

    public Dictionary<string, ZLinkChannelRegistration> Channels { get; } = new(StringComparer.Ordinal);

    public Dictionary<string, ZLinkStreamNodeRegistration> StreamNodes { get; } = new(StringComparer.Ordinal);

    public Dictionary<string, ZLinkSpotNodeRegistration> SpotNodes { get; } = new(StringComparer.Ordinal);

    public ZLinkActorCatalog ActorCatalog { get; } = new();

    public TimeSpan ResolveChannelRequestTimeout(string channelName)
    {
        return Channels.TryGetValue(channelName, out var channel)
            ? channel.DefaultRequestTimeout ?? DefaultRequestTimeout
            : DefaultRequestTimeout;
    }

    public TimeSpan ResolveMeshRequestTimeout(string meshName)
    {
        return SpotNodes.TryGetValue(meshName, out var node)
            ? node.DefaultRequestTimeout ?? DefaultRequestTimeout
            : DefaultRequestTimeout;
    }

    internal ulong ResolveMaximumApplicationMessageBytes()
    {
        long maximum = 0;
        foreach (var channel in Channels.Values)
        {
            if (channel.Server is not null)
                maximum = Math.Max(
                    maximum,
                    channel.Server.SocketConfig.MaxMessageSize);
            if (channel.Subscriber is not null)
                maximum = Math.Max(
                    maximum,
                    channel.Subscriber.SocketConfig.MaxMessageSize);
        }

        foreach (var node in SpotNodes.Values)
            if (node.Router is not null)
                maximum = Math.Max(
                    maximum,
                    node.Router.SocketConfig.MaxMessageSize);

        foreach (var streamNode in StreamNodes.Values)
            maximum = Math.Max(
                maximum,
                streamNode.SocketConfig.MaxMessageSize);

        return maximum > 0
            ? checked((ulong)maximum)
            : 16UL * 1024 * 1024;
    }

    public IEnumerable<Assembly> EnumerateHandlerScanAssemblies()
    {
        var assemblies = new HashSet<Assembly>(HandlerAssemblies);
        if (!ImplicitHandlerAutoRegistrationEnabled) return assemblies;

        foreach (var filterType in Filters) assemblies.Add(filterType.Assembly);

        foreach (var stream in StreamNodes.Values)
            if (stream.HeaderSessionType is not null)
                assemblies.Add(stream.HeaderSessionType.Assembly);

        foreach (var spotNode in SpotNodes.Values)
        {
            if (spotNode.EntrySpotType is not null) assemblies.Add(spotNode.EntrySpotType.Assembly);

            foreach (var spotType in spotNode.SpotFactories) assemblies.Add(spotType.Assembly);

            foreach (var instanceSpot in spotNode.InstanceSpotFactories.Values)
                assemblies.Add(instanceSpot.SpotType.Assembly);

            foreach (var actorFactoryType in spotNode.ActorFactories.Values) assemblies.Add(actorFactoryType.Assembly);

            foreach (var handler in spotNode.RouteSendHandlers) assemblies.Add(handler.HandlerType.Assembly);

            foreach (var handler in spotNode.RouteRequestHandlers) assemblies.Add(handler.HandlerType.Assembly);

            foreach (var membership in spotNode.ChannelMemberships)
            {
                foreach (var handler in membership.SendHandlers) assemblies.Add(handler.HandlerType.Assembly);

                foreach (var handler in membership.RequestHandlers) assemblies.Add(handler.HandlerType.Assembly);
            }
        }

        foreach (var channel in Channels.Values)
        {
            foreach (var handler in channel.SendHandlers) assemblies.Add(handler.HandlerType.Assembly);

            foreach (var handler in channel.RequestHandlers) assemblies.Add(handler.HandlerType.Assembly);

            foreach (var handler in channel.PublishHandlers) assemblies.Add(handler.HandlerType.Assembly);
        }

        return assemblies;
    }

    public ZLinkScannedHandlerCatalog ScannedHandlerCatalog =>
        _scannedHandlerCatalog ??= ZLinkScannedHandlerCatalog.Build(
            EnumerateHandlerScanAssemblies(),
            EnumerateSessionTypes());

    public void FreezeScannedHandlerCatalog()
    {
        _scannedHandlerCatalog = ZLinkScannedHandlerCatalog.Build(
            EnumerateHandlerScanAssemblies(),
            EnumerateSessionTypes());
    }

    private IReadOnlySet<Type> EnumerateSessionTypes() => StreamNodes.Values
        .Select(static node => node.HeaderSessionType)
        .Where(static type => type is not null)
        .Cast<Type>()
        .ToHashSet();
}

internal sealed record ZLinkScannedHandlerCatalog(
    IReadOnlyList<ZLinkHandlerEndpointDescriptor> ChannelEndpoints,
    IReadOnlyList<ZLinkScannedSpotHandler> SpotHandlers,
    IReadOnlyList<ZLinkScannedSessionHandler> SessionHandlers)
{
    public static ZLinkScannedHandlerCatalog Build(
        IEnumerable<Assembly> assemblies,
        IReadOnlySet<Type> sessionTypes)
    {
        var channelEndpoints = new List<ZLinkHandlerEndpointDescriptor>();
        var spotHandlers = new List<ZLinkScannedSpotHandler>();
        var sessionHandlers = new List<ZLinkScannedSessionHandler>();
        foreach (var assembly in assemblies)
        {
            channelEndpoints.AddRange(ZLinkHandlerScanner.Scan(assembly));
            spotHandlers.AddRange(ZLinkScannedSpotHandlerScanner.Scan(assembly));
            sessionHandlers.AddRange(ZLinkScannedSessionHandlerScanner.Scan(assembly, sessionTypes));
        }

        return new ZLinkScannedHandlerCatalog(
            Array.AsReadOnly(channelEndpoints.ToArray()),
            Array.AsReadOnly(spotHandlers.ToArray()),
            Array.AsReadOnly(sessionHandlers.ToArray()));
    }
}

internal sealed class ZLinkMetadataPolicyRegistration
{
    public HashSet<string> SessionToActorKeys { get; } = new(StringComparer.Ordinal);

    public HashSet<string> ActorToSessionKeys { get; } = new(StringComparer.Ordinal);
}

[Flags]
internal enum ZLinkClientServerRole
{
    Client = 1,
    Server = 2
}

// Mesh channel marker: AddRouteMesh(meshName) registers the mesh discovery
// name and the MeshNode references it through SpotMeshChannelName. Peer
// acquisition is owned by location-store auto-connect or manual wiring.
internal sealed class ZLinkChannelRegistration
{
    public required string ChannelName { get; init; }

    public TimeSpan? DefaultRequestTimeout { get; set; }

    public ZLinkLocationAutoConnectType AutoConnectType { get; set; }

    public ZLinkClientServerRole? ClientServerRole { get; set; }

    public bool HasClientServerClient =>
        (ClientServerRole.GetValueOrDefault() & ZLinkClientServerRole.Client) != 0;

    public bool HasClientServerServer =>
        (ClientServerRole.GetValueOrDefault() & ZLinkClientServerRole.Server) != 0;

    public ZLinkChannelServerCapabilityRegistration? Server { get; set; }

    public ZLinkChannelClientCapabilityRegistration? Client { get; set; }

    public ZLinkChannelPublisherCapabilityRegistration? Publisher { get; set; }

    public ZLinkChannelSubscriberCapabilityRegistration? Subscriber { get; set; }

    public RoutingId RoutingId { get; set; }

    public bool HasExplicitRoutingId { get; set; }

    public HashSet<string> HandlerGroups { get; } = new(StringComparer.Ordinal);

    public List<ZLinkChannelHandlerRegistration> SendHandlers { get; } = [];

    public List<ZLinkChannelHandlerRegistration> RequestHandlers { get; } = [];

    public List<ZLinkChannelHandlerRegistration> PublishHandlers { get; } = [];
}

internal sealed class ZLinkChannelServerCapabilityRegistration
{
    public RoutingId ServerRid { get; } =
        RoutingId.From($"cs-{Guid.NewGuid():N}");

    public int ListenPort { get; set; }

    public string? BindHost { get; set; }

    public string? AdvertiseHost { get; set; }

    public ZLinkSocketConfig SocketConfig { get; } = new();

    public ZLinkRouteConfig RoutingConfig { get; } = new();
}

internal sealed class ZLinkChannelClientCapabilityRegistration
{
    public ZLinkPeerAcquisitionMode AcquisitionMode { get; set; } =
        ZLinkPeerAcquisitionMode.Manual;

    public ZLinkSocketConfig SocketConfig { get; } = new();

    public ZLinkOutboundRouteConfig RoutingConfig { get; } = new();

    public ZLinkEndpointConnections ManualConnections { get; } = new();
}

internal sealed class ZLinkChannelPublisherCapabilityRegistration
{
    public string? BindEndpoint { get; set; }

    public int? ListenPort { get; set; }

    public string? BindHost { get; set; }

    public string? AdvertiseHost { get; set; }

    public RoutingId FixedRoutingId { get; set; }

    public string? RoutingIdPrefix { get; set; }

    public ZLinkSocketConfig SocketConfig { get; } = new();
}

internal sealed class ZLinkChannelSubscriberCapabilityRegistration
{
    public bool AutomaticDiscoveryEnabled { get; set; }

    public ZLinkPeerAcquisitionMode AcquisitionMode { get; set; } = ZLinkPeerAcquisitionMode.Manual;

    public ZLinkSocketConfig SocketConfig { get; } = new();

    public ZLinkEndpointConnections ManualConnections { get; } = new();
}

internal sealed class ZLinkStreamNodeRegistration
{
    public required string StreamNodeName { get; init; }

    public string? BindEndpoint { get; set; }

    public int? ListenPort { get; set; }

    public string? BindHost { get; set; }

    public string? AdvertiseHost { get; set; }

    public ZLinkSocketConfig SocketConfig { get; } =
        new(ZLinkSocketConfig.DefaultStreamMaxMessageSize);

    public bool ActorDispatchEnabled { get; set; }

    public ZLinkStreamTlsServerRegistration? TlsServer { get; set; }

    public Type? HeaderSessionType { get; set; }
}

internal sealed record ZLinkStreamTlsServerRegistration(
    string CertPath,
    string KeyPath,
    bool RequireClientCert);

internal sealed record ZLinkRouteHandlerRegistration(
    Type HandlerType,
    Type MessageType,
    Type? ReplyType,
    string? PacketName);

internal sealed record ZLinkChannelHandlerRegistration(
    Type HandlerType,
    Type MessageType,
    Type? ReplyType,
    string? PacketName);

// One logical channel membership on a MeshNode (spec 05-route-mesh §4). The
// membership is (MeshName, ChannelName) scoped; weight is 0..10000 (0 excludes the
// channel from new select-one/multicast targeting). Handlers are the channel's
// IZLinkSendHandler/IZLinkRequestHandler namespace.
internal sealed class ZLinkMeshChannelMembership
{
    public required string ChannelName { get; init; }

    public bool IsServer { get; init; } = true;

    private int _weight = ZLinkSocketConfig.DefaultPeerWeight;

    public int Weight
    {
        get => _weight;
        set
        {
            ZLinkSocketConfig.ValidatePeerWeight(value);
            _weight = value;
        }
    }

    public List<ZLinkChannelHandlerRegistration> SendHandlers { get; } = [];

    public List<ZLinkChannelHandlerRegistration> RequestHandlers { get; } = [];

    public HashSet<string> HandlerGroups { get; } = new(StringComparer.Ordinal);
}

internal sealed class ZLinkSpotNodeRegistration
{
    public required string SpotNodeName { get; init; }

    public string? SpotMeshChannelName { get; set; }

    public ZLinkSpotRouterCapabilityRegistration? Router { get; set; }

    // MeshNode-level default for RID-direct route requests handled by this node
    // (spec 05-route-mesh §2 IZLinkMeshNodeBuilder.SetDefaultRequestTimeout). Null
    // falls back to the framework-wide DefaultRequestTimeout.
    public TimeSpan? DefaultRequestTimeout { get; set; }

    // Logical Multicast publisher transport settings for this MeshNode
    // (spec §5 IZLinkSpotPublisherConfig, ConfigureSpotPublisher).
    public ZLinkSpotPublisherConfig SpotPublisherConfig { get; } = new();

    // Immutable logical channel memberships added via ChannelName(...) (spec §4).
    // Each carries its build-time weight and channel-scoped handler namespace.
    public List<ZLinkMeshChannelMembership> ChannelMemberships { get; } = [];

    // RID-direct route handlers registered on the MeshNode builder itself
    // (spec §2 AddRouteSendHandler/AddRouteRequestHandler). They share the node
    // ROUTER's source-RID route-handler context.
    public List<ZLinkRouteHandlerRegistration> RouteSendHandlers { get; } = [];

    public List<ZLinkRouteHandlerRegistration> RouteRequestHandlers { get; } = [];

    public HashSet<Type> SpotFactories { get; } = [];

    public Dictionary<Type, ZLinkUserSpotFactoryConfiguration> UserSpotFactoryOptions { get; } = [];

    public Dictionary<string, ZLinkInstanceSpotFactoryRegistration>
        InstanceSpotFactories { get; } = new(StringComparer.Ordinal);

    public Dictionary<string, Type> ActorFactories { get; } = new(StringComparer.Ordinal);

    public Dictionary<string, ZLinkObjectRelocationRegistration>
        SpotRelocations { get; } = new(StringComparer.Ordinal);

    public Dictionary<string, ZLinkObjectRelocationRegistration>
        InstanceSpotRelocations { get; } = new(StringComparer.Ordinal);

    public Dictionary<string, ZLinkObjectRelocationRegistration>
        ActorRelocations { get; } = new(StringComparer.Ordinal);

    public RoutingId RoutingId { get; set; }

    public bool HasExplicitRoutingId { get; set; }

    public string? RoutingIdPrefix { get; set; }

    public RoutingId PreparedRoutingId { get; set; }

    internal RoutingId EffectiveRoutingId =>
        PreparedRoutingId.Size > 0 ? PreparedRoutingId : RoutingId;

    public string EntrySpotId { get; set; } = string.Empty;

    public Type? EntrySpotType { get; set; }

    public ZLinkMeshNodeObjectRole ObjectRole { get; set; }

    public bool ObjectRoleSelected { get; set; }

    public int PlacementWeight { get; set; } = 100;

    public int ActorLimit { get; set; }

    public int SpotLimit { get; set; }

    public int ActivationConcurrencyLimit { get; set; } = 128;

    public int MaxActiveObjects { get; set; } = 10_000;

    public int MaxPendingActivations { get; set; } = 128;

    public TimeSpan InstanceSpotIdleTimeout { get; set; }
}

internal sealed record ZLinkInstanceSpotFactoryRegistration(
    Type SpotType,
    ZLinkInstanceSpotFactoryConfiguration Options);

internal sealed record ZLinkUserSpotFactoryConfiguration(
    int StableTypeLimit = 0,
    ZLinkUserSpotExecutionMode ExecutionMode = ZLinkUserSpotExecutionMode.SpotWide,
    ZLinkSpotRelocationReadinessMode RelocationReadiness =
        ZLinkSpotRelocationReadinessMode.AnyTurnBoundary);

internal sealed record ZLinkInstanceSpotFactoryConfiguration(
    int StableTypeLimit = 0);

internal sealed record ZLinkObjectRelocationRegistration(
    Type InstanceType,
    ZLinkObjectPlacementOptions Placement,
    byte PolicyKind,
    Type? AdapterType,
    IZLinkRelocationAdapterInvoker? AdapterInvoker);

internal sealed record ZLinkObjectPlacementOptions
{
    public int? MaxActiveObjects { get; init; }

    public int? MaxPendingActivations { get; init; }
}

internal sealed class ZLinkActorCatalog
{
    private IReadOnlyDictionary<string, Type> _factories = FrozenDictionary<string, Type>.Empty;
    public IReadOnlyDictionary<string, Type> Factories => _factories;

    public void Build(IEnumerable<ZLinkSpotNodeRegistration> spotNodes)
    {
        var factories = new Dictionary<string, Type>(StringComparer.Ordinal);
        foreach (var spotNode in spotNodes)
        {
            foreach (var (actorType, factoryType) in spotNode.ActorFactories)
                factories.TryAdd(actorType, factoryType);
        }

        _factories = factories.ToFrozenDictionary(StringComparer.Ordinal);
    }

    public Type ResolveFactory(string actorType)
    {
        return _factories.TryGetValue(actorType, out var factoryType)
            ? factoryType
            : throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                $"Actor factory '{actorType}' is not registered.");
    }
}

internal sealed class ZLinkSpotRouterCapabilityRegistration
{
    public ZLinkPeerAcquisitionMode AcquisitionMode { get; set; } = ZLinkPeerAcquisitionMode.Manual;

    public string? BindEndpoint { get; set; }

    public int? ListenPort { get; set; }

    public string? BindHost { get; set; }

    public string? AdvertiseHost { get; set; }

    public ZLinkSocketConfig SocketConfig { get; } = new();

    public ZLinkRouteConfig RoutingConfig { get; } = new();

    public ZLinkEndpointConnections ManualConnections { get; } = new();

    public Dictionary<string, RoutingId> PeerRoutingIds { get; } = new(StringComparer.Ordinal);
}

internal sealed class ZLinkNetworkOptionsModel : IZLinkNetworkOptions
{
    private string _bindHost = "127.0.0.1";
    private string? _advertiseHost;

    public string BindHost
    {
        get => _bindHost;
        set => _bindHost = ValidateHost(value, nameof(BindHost));
    }

    public string? AdvertiseHost
    {
        get => _advertiseHost;
        set => _advertiseHost = value is null
            ? null
            : ValidateHost(value, nameof(AdvertiseHost));
    }

    private static string ValidateHost(string host, string name)
    {
        if (string.IsNullOrWhiteSpace(host))
            throw new ZLinkConfigurationException($"{name} must not be empty.");
        return host;
    }
}
