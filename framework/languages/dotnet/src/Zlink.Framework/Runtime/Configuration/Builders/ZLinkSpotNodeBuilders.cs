namespace Zlink.Framework.Runtime.Configuration.Builders;

// MeshNode builder (spec 05-route-mesh §2). Drives the same
// ZLinkSpotNodeRegistration the runtime consumes: AddRouteMesh(meshName) owns the
// node's single ROUTER endpoint, its logical channel memberships, RID-direct route
// handlers, Spot/Actor registry, publish admission policy and drain policy.
internal sealed class ZLinkMeshNodeBuilder(ZLinkSpotNodeRegistration registration)
    : IZLinkMeshNodeBuilder
{
    private ZLinkMeshPeerConnections? _peerConnections;

    public IZLinkMeshChannelRoleBuilder Channel(string channelName)
    {
        ValidateChannelName(channelName);
        return new ZLinkMeshChannelRoleBuilder(registration, channelName);
    }

    public IZLinkMeshNodeBuilder Listen(string endpoint)
    {
        if (string.IsNullOrWhiteSpace(endpoint))
            throw new ZLinkConfigurationException("MeshNode ROUTER bind endpoint must not be empty.");

        EnsureRouter().BindEndpoint = endpoint;
        EnsureRouter().ListenPort = null;
        return this;
    }

    public IZLinkMeshNodeBuilder Listen(int port = 0)
    {
        if (port is < 0 or > 65535)
            throw new ZLinkConfigurationException(
                "MeshNode listen port must be between 0 and 65535.");
        var router = EnsureRouter();
        router.ListenPort = port;
        router.BindEndpoint = null;
        return this;
    }

    public IZLinkMeshNodeBuilder SetBindHost(string bindHost)
    {
        EnsureRouter().BindHost = ZLinkChannelEndpointBuilderSupport.Validate(
            bindHost,
            "MeshNode bind host must not be empty.");
        return this;
    }

    public IZLinkMeshNodeBuilder SetAdvertiseHost(string advertiseHost)
    {
        EnsureRouter().AdvertiseHost = ZLinkChannelEndpointBuilderSupport.Validate(
            advertiseHost,
            "MeshNode advertise host must not be empty.");
        return this;
    }

    public IZLinkMeshNodeBuilder SetRoutingId(RoutingId routingId)
    {
        if (registration.HasExplicitRoutingId
            || registration.RoutingIdPrefix is not null)
            throw new ZLinkConfigurationException(
                "MeshNode routing mode can be configured only once.");
        if (routingId.Size == 0)
            throw new ZLinkConfigurationException(
                "MeshNode routing ID must not be empty.");
        registration.RoutingId = routingId;
        registration.HasExplicitRoutingId = true;
        return this;
    }

    public IZLinkMeshNodeBuilder SetRoutingIdPrefix(string prefix)
    {
        if (registration.HasExplicitRoutingId
            || registration.RoutingIdPrefix is not null)
            throw new ZLinkConfigurationException(
                "MeshNode routing mode can be configured only once.");
        if (string.IsNullOrEmpty(prefix)
            || prefix.Length > 64
            || prefix.Any(static character =>
                !((character >= 'A' && character <= 'Z')
                  || (character >= 'a' && character <= 'z')
                  || (character >= '0' && character <= '9')
                  || character is '.' or '_' or '-')))
            throw new ZLinkConfigurationException(
                "MeshNode routing-id prefix must contain 1 to 64 ASCII "
                + "letters, digits, '.', '_' or '-'.");
        registration.RoutingIdPrefix = prefix;
        return this;
    }

    public IZLinkMeshNodeBuilder SetPlacementWeight(int weight)
    {
        ZLinkSocketConfig.ValidatePeerWeight(weight);
        registration.PlacementWeight = weight;
        return this;
    }

    public IZLinkMeshNodeBuilder SetActorLimit(int limit)
    {
        ValidatePopulationLimit(limit, "Actor limit");
        registration.ActorLimit = limit;
        return this;
    }

    public IZLinkMeshNodeBuilder SetSpotLimit(int limit)
    {
        ValidatePopulationLimit(limit, "Spot limit");
        registration.SpotLimit = limit;
        return this;
    }

    public IZLinkMeshNodeBuilder SetActivationConcurrency(int limit)
    {
        ValidatePositiveLimit(limit, "Activation concurrency");
        registration.ActivationConcurrencyLimit = limit;
        registration.MaxPendingActivations = limit;
        return this;
    }

    public IZLinkMeshNodeBuilder SetInstanceSpotIdleTimeout(TimeSpan timeout)
    {
        if (timeout < TimeSpan.Zero)
            throw new ZLinkConfigurationException(
                "Instance Spot idle timeout must not be negative.");
        registration.InstanceSpotIdleTimeout = timeout;
        return this;
    }

    public IZLinkMeshObjectRoleBuilder Objects() =>
        new ZLinkMeshObjectRoleBuilder(registration);

    public IZLinkMeshNodeSocketConfig ConfigureRouterSocket() => EnsureRouter().SocketConfig;

    public IZLinkSpotPublisherConfig ConfigureSpotPublisher() => registration.SpotPublisherConfig;

    public IZLinkMeshPeerConnections PeerConnections =>
        _peerConnections ??= new ZLinkMeshPeerConnections(EnsureRouter());

    public IZLinkMeshNodeBuilder SetDefaultRequestTimeout(TimeSpan timeout)
    {
        ZLinkRequestTimeoutValidation.Validate(timeout, nameof(timeout));
        registration.DefaultRequestTimeout = timeout;
        return this;
    }

    public IZLinkMeshNodeBuilder AddRouteSendHandler<THandler, TMessage>(string? packetName = null)
        where THandler : class, IZLinkRouteSendHandler<TMessage>
    {
        registration.RouteSendHandlers.Add(new ZLinkRouteHandlerRegistration(
            typeof(THandler),
            typeof(TMessage),
            null,
            packetName));
        return this;
    }

    public IZLinkMeshNodeBuilder AddRouteSendHandler<THandler>(string? packetName = null)
        where THandler : class
    {
        var args = ZLinkTypedHandlerBuilderSupport.ResolveSingleHandlerInterface(
                typeof(THandler),
                typeof(IZLinkRouteSendHandler<>),
                "route send")
            .GetGenericArguments();
        registration.RouteSendHandlers.Add(new ZLinkRouteHandlerRegistration(
            typeof(THandler),
            args[0],
            null,
            packetName));
        return this;
    }

    public IZLinkMeshNodeBuilder AddRouteRequestHandler<THandler, TRequest, TReply>(string? packetName = null)
        where THandler : class, IZLinkRouteRequestHandler<TRequest, TReply>
    {
        registration.RouteRequestHandlers.Add(new ZLinkRouteHandlerRegistration(
            typeof(THandler),
            typeof(TRequest),
            typeof(TReply),
            packetName));
        return this;
    }

    public IZLinkMeshNodeBuilder AddRouteRequestHandler<THandler>(string? packetName = null)
        where THandler : class
    {
        var args = ZLinkTypedHandlerBuilderSupport.ResolveSingleHandlerInterface(
                typeof(THandler),
                typeof(IZLinkRouteRequestHandler<,>),
                "route request")
            .GetGenericArguments();
        registration.RouteRequestHandlers.Add(new ZLinkRouteHandlerRegistration(
            typeof(THandler),
            args[0],
            args[1],
            packetName));
        return this;
    }

    internal void RegisterSpotFactory<TSpot>(
        string spotType,
        Action<IZLinkUserSpotFactoryBuilder<TSpot>> configure)
        where TSpot : class, IZLinkSpot
    {
        EnsureServerRole();
        ValidateObjectType(spotType, "Spot");
        ArgumentNullException.ThrowIfNull(configure);
        var factory = new ZLinkUserSpotFactoryBuilder<TSpot>();
        configure(factory);
        factory.CompleteConfiguration();
        var effectiveOptions = factory.Configuration;
        ValidateUserSpotFactoryOptions(effectiveOptions);
        if (effectiveOptions.ExecutionMode == ZLinkUserSpotExecutionMode.PerActor
            && factory.Relocation.PolicyKind != 1)
            throw new ZLinkConfigurationException(
                "PerActor User Spots must use RecreateOnRelocation.");
        if (!registration.SpotFactories.Add(typeof(TSpot)))
            throw new ZLinkConfigurationException(
                $"Duplicate SPOT factory '{typeof(TSpot)}' on MeshNode '{registration.SpotNodeName}'.");
        registration.UserSpotFactoryOptions.Add(typeof(TSpot), effectiveOptions);
        AddRelocation(
            registration.SpotRelocations,
            spotType,
            typeof(TSpot),
            effectiveOptions.StableTypeLimit == 0
                ? null
                : new ZLinkObjectPlacementOptions
                {
                    MaxActiveObjects = effectiveOptions.StableTypeLimit
                },
            factory.Relocation);
    }

    internal void RegisterInstanceSpotFactory<TSpot>(
        string instanceSpotType,
        Action<IZLinkInstanceSpotFactoryBuilder<TSpot>> configure)
        where TSpot : class, IZLinkInstanceSpot
    {
        EnsureServerRole();
        ValidateObjectType(instanceSpotType, "Instance Spot");
        ArgumentNullException.ThrowIfNull(configure);
        var factory = new ZLinkInstanceSpotFactoryBuilder<TSpot>();
        configure(factory);
        factory.CompleteConfiguration();
        if (!registration.InstanceSpotFactories.TryAdd(
                instanceSpotType,
                new ZLinkInstanceSpotFactoryRegistration(
                    typeof(TSpot),
                    factory.Configuration)))
            throw new ZLinkConfigurationException(
                $"Duplicate Instance Spot factory '{instanceSpotType}' on "
                + $"MeshNode '{registration.SpotNodeName}'.");
        var effectiveOptions = factory.Configuration;
        ValidateStableTypeLimit(effectiveOptions.StableTypeLimit, "Instance Spot");
        AddRelocation(
            registration.InstanceSpotRelocations,
            instanceSpotType,
            typeof(TSpot),
            PlacementFromStableTypeLimit(effectiveOptions.StableTypeLimit),
            factory.Relocation);
    }

    internal void RegisterEntrySpot<TEntrySpot>()
        where TEntrySpot : IZLinkEntrySpot
    {
        EnsureServerRole();
        if (registration.EntrySpotType is not null)
            throw new ZLinkConfigurationException(
                $"Duplicate Entry Spot registry on MeshNode '{registration.SpotNodeName}'.");

        registration.EntrySpotType = typeof(TEntrySpot);
    }

    internal void RegisterActorFactory<TActor, TFactory>(
        string actorType,
        Action<IZLinkActorFactoryBuilder<TActor>> configure)
        where TActor : class, IZLinkActor
        where TFactory : class, IZLinkActorFactory<TActor>
    {
        EnsureServerRole();
        ArgumentNullException.ThrowIfNull(configure);
        var factory = new ZLinkActorFactoryBuilder<TActor>();
        configure(factory);
        factory.CompleteConfiguration();
        ZLinkRegistrationBuilderGuard.AddUnique(
            registration.ActorFactories,
            actorType,
            typeof(TFactory),
            "Actor factory name must not be empty.",
            $"Duplicate actor factory '{actorType}'.");
        AddRelocation(
            registration.ActorRelocations,
            actorType,
            typeof(TActor),
            null,
            factory.Relocation);
    }

    private ZLinkSpotRouterCapabilityRegistration EnsureRouter()
    {
        registration.Router ??= new ZLinkSpotRouterCapabilityRegistration();
        return registration.Router;
    }

    private static void ValidateChannelName(string channelName)
    {
        if (string.IsNullOrWhiteSpace(channelName))
            throw new ZLinkConfigurationException("Channel membership name must not be empty.");
        if (System.Text.Encoding.UTF8.GetByteCount(channelName) > 255
            || channelName.Contains('\0'))
            throw new ZLinkConfigurationException(
                "Channel membership name must be 1 to 255 UTF-8 bytes without NUL.");
    }

    internal static ZLinkMeshChannelMembership AddChannelMembership(
        ZLinkSpotNodeRegistration registration,
        string channelName,
        bool isServer)
    {
        ValidateChannelName(channelName);
        if (registration.ChannelMemberships.Any(
                membership => string.Equals(
                    membership.ChannelName,
                    channelName,
                    StringComparison.Ordinal)))
            throw new ZLinkConfigurationException(
                $"Duplicate channel membership '{channelName}' on MeshNode '{registration.SpotNodeName}'.");

        var membership = new ZLinkMeshChannelMembership
        {
            ChannelName = channelName,
            IsServer = isServer
        };
        registration.ChannelMemberships.Add(membership);
        return membership;
    }

    private void EnsureServerRole()
    {
        if (registration.ObjectRole == ZLinkMeshNodeObjectRole.Client)
            throw new ZLinkConfigurationException(
                $"MeshNode '{registration.SpotNodeName}' is configured as an Object Client.");
        registration.ObjectRole = ZLinkMeshNodeObjectRole.Server;
        registration.ObjectRoleSelected = true;
    }

    private static void AddRelocation(
        IDictionary<string, ZLinkObjectRelocationRegistration> target,
        string stableType,
        Type instanceType,
        ZLinkObjectPlacementOptions? placement,
        ZLinkRelocationFactoryConfiguration relocation)
    {
        var effectivePlacement = placement ?? new ZLinkObjectPlacementOptions();
        ValidatePlacement(effectivePlacement);
        if (!target.TryAdd(
                stableType,
                new ZLinkObjectRelocationRegistration(
                    instanceType,
                    effectivePlacement,
                    relocation.PolicyKind,
                    relocation.AdapterType,
                    relocation.AdapterInvoker)))
            throw new ZLinkConfigurationException(
                $"Duplicate relocation registration '{stableType}'.");
    }

    private static void ValidateObjectType(string stableType, string kind)
    {
        if (string.IsNullOrWhiteSpace(stableType))
            throw new ZLinkConfigurationException(
                $"{kind} type must not be empty.");
        if (System.Text.Encoding.UTF8.GetByteCount(stableType) > 255
            || stableType.Contains('\0'))
            throw new ZLinkConfigurationException(
                $"{kind} type must be 1 to 255 UTF-8 bytes without NUL.");
    }

    private static void ValidatePositiveLimit(int limit, string name)
    {
        if (limit <= 0)
            throw new ZLinkConfigurationException($"{name} must be greater than zero.");
    }

    private static void ValidatePopulationLimit(int limit, string name)
    {
        if (limit < 0)
            throw new ZLinkConfigurationException(
                $"{name} must be zero (unlimited) or greater.");
    }

    private static void ValidatePlacement(ZLinkObjectPlacementOptions placement)
    {
        if (placement.MaxActiveObjects is <= 0)
            throw new ZLinkConfigurationException(
                "MaxActiveObjects must be greater than zero.");
        if (placement.MaxPendingActivations is <= 0)
            throw new ZLinkConfigurationException(
                "MaxPendingActivations must be greater than zero.");
    }

    private static void ValidateUserSpotFactoryOptions(
        ZLinkUserSpotFactoryConfiguration options)
    {
        if (options.StableTypeLimit < 0)
            throw new ZLinkConfigurationException(
                "User Spot StableTypeLimit must not be negative.");
        if (!Enum.IsDefined(options.ExecutionMode))
            throw new ZLinkConfigurationException(
                "User Spot execution mode is not supported.");
        if (!Enum.IsDefined(options.RelocationReadiness))
            throw new ZLinkConfigurationException(
                "User Spot relocation readiness mode is not supported.");
        if (options.ExecutionMode != ZLinkUserSpotExecutionMode.SpotWide
            && options.RelocationReadiness
            == ZLinkSpotRelocationReadinessMode.ApplicationSignaled)
            throw new ZLinkConfigurationException(
                "ApplicationSignaled relocation readiness is valid only for SpotWide User Spots.");
    }

    private static void ValidateStableTypeLimit(int limit, string kind)
    {
        if (limit < 0)
            throw new ZLinkConfigurationException(
                $"{kind} StableTypeLimit must not be negative.");
    }

    private static ZLinkObjectPlacementOptions? PlacementFromStableTypeLimit(int limit) =>
        limit == 0
            ? null
            : new ZLinkObjectPlacementOptions { MaxActiveObjects = limit };
}

internal sealed record ZLinkRelocationFactoryConfiguration(
    byte PolicyKind,
    Type? AdapterType,
    IZLinkRelocationAdapterInvoker? AdapterInvoker);

internal abstract class ZLinkFactoryBuilderBase
{
    private ZLinkRelocationFactoryConfiguration? _relocation;
    private bool _configurationComplete;

    internal ZLinkRelocationFactoryConfiguration Relocation =>
        _relocation
        ?? throw new ZLinkConfigurationException(
            "A factory must configure exactly one relocation policy.");

    internal void CompleteConfiguration()
    {
        _ = Relocation;
        _configurationComplete = true;
    }

    protected void EnsureConfigurationIsOpen()
    {
        if (_configurationComplete)
            throw new ZLinkConfigurationException(
                "A factory builder cannot be changed after its configure callback returns.");
    }

    protected void SelectRelocation(
        byte policyKind,
        Type? adapterType = null,
        IZLinkRelocationAdapterInvoker? adapterInvoker = null)
    {
        EnsureConfigurationIsOpen();
        if (_relocation is not null)
            throw new ZLinkConfigurationException(
                "A factory must configure exactly one relocation policy.");
        _relocation = new ZLinkRelocationFactoryConfiguration(
            policyKind,
            adapterType,
            adapterInvoker);
    }
}

internal sealed class ZLinkActorFactoryBuilder<TActor>
    : ZLinkFactoryBuilderBase, IZLinkActorFactoryBuilder<TActor>
    where TActor : class, IZLinkActor
{
    public IZLinkActorFactoryBuilder<TActor> DisableRelocation()
    {
        SelectRelocation(0);
        return this;
    }

    public IZLinkActorFactoryBuilder<TActor> RecreateOnRelocation()
    {
        SelectRelocation(1);
        return this;
    }

    public IZLinkActorFactoryBuilder<TActor> PreserveStateWith<TAdapter>()
        where TAdapter : class, IZLinkActorRelocationAdapter<TActor>
    {
        SelectRelocation(
            2,
            typeof(TAdapter),
            new ZLinkActorRelocationAdapterInvoker<TActor>(typeof(TAdapter)));
        return this;
    }
}

internal sealed class ZLinkUserSpotFactoryBuilder<TSpot>
    : ZLinkFactoryBuilderBase, IZLinkUserSpotFactoryBuilder<TSpot>
    where TSpot : class, IZLinkSpot
{
    private int _stableTypeLimit;
    private ZLinkUserSpotExecutionMode _executionMode =
        ZLinkUserSpotExecutionMode.SpotWide;
    private ZLinkSpotRelocationReadinessMode _relocationReadiness =
        ZLinkSpotRelocationReadinessMode.AnyTurnBoundary;

    internal ZLinkUserSpotFactoryConfiguration Configuration =>
        new(_stableTypeLimit, _executionMode, _relocationReadiness);

    public IZLinkUserSpotFactoryBuilder<TSpot> StableTypeLimit(int limit)
    {
        EnsureConfigurationIsOpen();
        if (limit <= 0)
            throw new ZLinkConfigurationException(
                "User Spot StableTypeLimit must be greater than zero.");
        _stableTypeLimit = limit;
        return this;
    }

    public IZLinkUserSpotFactoryBuilder<TSpot> ExecutionMode(
        ZLinkUserSpotExecutionMode mode)
    {
        EnsureConfigurationIsOpen();
        _executionMode = mode;
        return this;
    }

    public IZLinkUserSpotFactoryBuilder<TSpot> RelocationReadiness(
        ZLinkSpotRelocationReadinessMode mode)
    {
        EnsureConfigurationIsOpen();
        _relocationReadiness = mode;
        return this;
    }

    public IZLinkUserSpotFactoryBuilder<TSpot> DisableRelocation()
    {
        SelectRelocation(0);
        return this;
    }

    public IZLinkUserSpotFactoryBuilder<TSpot> RecreateOnRelocation()
    {
        SelectRelocation(1);
        return this;
    }

    public IZLinkUserSpotFactoryBuilder<TSpot> PreserveStateWith<TAdapter>()
        where TAdapter : class, IZLinkSpotRelocationAdapter<TSpot>
    {
        SelectRelocation(
            2,
            typeof(TAdapter),
            new ZLinkSpotRelocationAdapterInvoker<TSpot>(typeof(TAdapter)));
        return this;
    }
}

internal sealed class ZLinkInstanceSpotFactoryBuilder<TSpot>
    : ZLinkFactoryBuilderBase, IZLinkInstanceSpotFactoryBuilder<TSpot>
    where TSpot : class, IZLinkInstanceSpot
{
    private int _stableTypeLimit;

    internal ZLinkInstanceSpotFactoryConfiguration Configuration =>
        new(_stableTypeLimit);

    public IZLinkInstanceSpotFactoryBuilder<TSpot> StableTypeLimit(int limit)
    {
        EnsureConfigurationIsOpen();
        if (limit <= 0)
            throw new ZLinkConfigurationException(
                "Instance Spot StableTypeLimit must be greater than zero.");
        _stableTypeLimit = limit;
        return this;
    }

    public IZLinkInstanceSpotFactoryBuilder<TSpot> DisableRelocation()
    {
        SelectRelocation(0);
        return this;
    }

    public IZLinkInstanceSpotFactoryBuilder<TSpot> RecreateOnRelocation()
    {
        SelectRelocation(1);
        return this;
    }

    public IZLinkInstanceSpotFactoryBuilder<TSpot> PreserveStateWith<TAdapter>()
        where TAdapter : class, IZLinkSpotRelocationAdapter<TSpot>
    {
        SelectRelocation(
            2,
            typeof(TAdapter),
            new ZLinkSpotRelocationAdapterInvoker<TSpot>(typeof(TAdapter)));
        return this;
    }
}

internal sealed class ZLinkMeshObjectRoleBuilder(
    ZLinkSpotNodeRegistration registration)
    : IZLinkMeshObjectRoleBuilder
{
    public IZLinkMeshObjectClientBuilder Client()
    {
        Select(ZLinkMeshNodeObjectRole.Client);
        return new ZLinkMeshObjectClientBuilder();
    }

    public IZLinkMeshObjectServerBuilder Server()
    {
        Select(ZLinkMeshNodeObjectRole.Server);
        return new ZLinkMeshObjectServerBuilder(registration);
    }

    private void Select(ZLinkMeshNodeObjectRole role)
    {
        if (registration.ObjectRoleSelected)
            throw new ZLinkConfigurationException(
                $"Object role for MeshNode '{registration.SpotNodeName}' was already selected.");
        registration.ObjectRole = role;
        registration.ObjectRoleSelected = true;
    }
}

internal sealed class ZLinkMeshObjectClientBuilder
    : IZLinkMeshObjectClientBuilder;

internal sealed class ZLinkMeshObjectServerBuilder(
    ZLinkSpotNodeRegistration registration)
    : IZLinkMeshObjectServerBuilder
{
    private readonly ZLinkMeshNodeBuilder _builder = new(registration);

    public IZLinkMeshObjectServerBuilder AddEntrySpot<TEntrySpot>()
        where TEntrySpot : class, IZLinkEntrySpot
    {
        _builder.RegisterEntrySpot<TEntrySpot>();
        return this;
    }

    public IZLinkMeshObjectServerBuilder AddSpotFactory<TSpot>(
        string spotType,
        Action<IZLinkUserSpotFactoryBuilder<TSpot>> configure)
        where TSpot : class, IZLinkSpot
    {
        _builder.RegisterSpotFactory(spotType, configure);
        return this;
    }

    public IZLinkMeshObjectServerBuilder AddInstanceSpotFactory<TSpot>(
        string instanceSpotType,
        Action<IZLinkInstanceSpotFactoryBuilder<TSpot>> configure)
        where TSpot : class, IZLinkInstanceSpot
    {
        _builder.RegisterInstanceSpotFactory(
            instanceSpotType,
            configure);
        return this;
    }

    public IZLinkMeshObjectServerBuilder AddActorFactory<TActor, TFactory>(
        string actorType,
        Action<IZLinkActorFactoryBuilder<TActor>> configure)
        where TActor : class, IZLinkActor
        where TFactory : class, IZLinkActorFactory<TActor>
    {
        _builder.RegisterActorFactory<TActor, TFactory>(
            actorType,
            configure);
        return this;
    }
}

internal sealed class ZLinkMeshChannelRoleBuilder(
    ZLinkSpotNodeRegistration registration,
    string channelName) : IZLinkMeshChannelRoleBuilder
{
    private bool _selected;

    public IZLinkMeshChannelClientBuilder Client()
    {
        var membership = Select(isServer: false);
        return new ZLinkMeshChannelClientBuilder(membership);
    }

    public IZLinkMeshChannelServerBuilder Server()
    {
        var membership = Select(isServer: true);
        return new ZLinkMeshChannelBuilder(membership);
    }

    private ZLinkMeshChannelMembership Select(bool isServer)
    {
        if (_selected)
            throw new ZLinkConfigurationException(
                $"Channel membership '{channelName}' already selected a role.");
        _selected = true;
        return ZLinkMeshNodeBuilder.AddChannelMembership(
            registration,
            channelName,
            isServer);
    }
}

internal sealed class ZLinkMeshChannelClientBuilder(
    ZLinkMeshChannelMembership membership) : IZLinkMeshChannelClientBuilder
{
    // Keep the selected registration strongly owned by this builder until the
    // configuration callback returns; no additional Client settings exist.
    private readonly ZLinkMeshChannelMembership _membership = membership;
}

// Logical channel membership builder (spec 05-route-mesh §4). Weight and the
// channel-scoped IZLinkSendHandler/IZLinkRequestHandler namespace are recorded on
// the membership the MeshNode owns.
internal sealed class ZLinkMeshChannelBuilder(ZLinkMeshChannelMembership membership)
    : IZLinkMeshChannelServerBuilder
{
    public IZLinkMeshChannelServerBuilder SetWeight(int weight)
    {
        ZLinkSocketConfig.ValidatePeerWeight(weight);
        membership.Weight = weight;
        return this;
    }

    public IZLinkMeshChannelServerBuilder AddHandlerGroup(string groupName)
    {
        ZLinkHandlerGroupBuilderSupport.AddHandlerGroup(membership.HandlerGroups, groupName);
        return this;
    }

    public IZLinkMeshChannelServerBuilder AddSendHandler<THandler, TMessage>(string? packetName = null)
        where THandler : class, IZLinkSendHandler<TMessage>
    {
        membership.SendHandlers.Add(new ZLinkChannelHandlerRegistration(
            typeof(THandler),
            typeof(TMessage),
            null,
            packetName));
        return this;
    }

    public IZLinkMeshChannelServerBuilder AddSendHandler<THandler>(string? packetName = null)
        where THandler : class
    {
        var args = ZLinkTypedHandlerBuilderSupport.ResolveSingleHandlerInterface(
                typeof(THandler),
                typeof(IZLinkSendHandler<>),
                "send")
            .GetGenericArguments();
        membership.SendHandlers.Add(new ZLinkChannelHandlerRegistration(
            typeof(THandler),
            args[0],
            null,
            packetName));
        return this;
    }

    public IZLinkMeshChannelServerBuilder AddRequestHandler<THandler, TRequest, TReply>(string? packetName = null)
        where THandler : class, IZLinkRequestHandler<TRequest, TReply>
    {
        membership.RequestHandlers.Add(new ZLinkChannelHandlerRegistration(
            typeof(THandler),
            typeof(TRequest),
            typeof(TReply),
            packetName));
        return this;
    }

    public IZLinkMeshChannelServerBuilder AddRequestHandler<THandler>(string? packetName = null)
        where THandler : class
    {
        var args = ZLinkTypedHandlerBuilderSupport.ResolveSingleHandlerInterface(
                typeof(THandler),
                typeof(IZLinkRequestHandler<,>),
                "request")
            .GetGenericArguments();
        membership.RequestHandlers.Add(new ZLinkChannelHandlerRegistration(
            typeof(THandler),
            args[0],
            args[1],
            packetName));
        return this;
    }

}

// Adapter over the MeshNode ROUTER's manual connection set and expected peer RIDs
// (spec 05-route-mesh §3 IZLinkMeshPeerConnections). Expected RID is optional; when
// present it pins the admission handshake identity for that endpoint.
internal sealed class ZLinkMeshPeerConnections(ZLinkSpotRouterCapabilityRegistration router)
    : IZLinkMeshPeerConnections
{
    public void Connect(string endpoint)
    {
        ValidateEndpoint(endpoint);
        router.ManualConnections.Connect(endpoint);
    }

    public void Connect(RoutingId expectedRoutingId, string endpoint)
    {
        if (expectedRoutingId.Size == 0)
            throw new ZLinkConfigurationException("Expected peer routing id must not be empty.");

        ValidateEndpoint(endpoint);
        if (router.PeerRoutingIds.TryGetValue(endpoint, out var previousRid)
            && previousRid != expectedRoutingId
            && router.ManualConnections.ListConnections().Contains(endpoint, StringComparer.Ordinal))
            router.ManualConnections.Disconnect(endpoint);
        router.PeerRoutingIds[endpoint] = expectedRoutingId;
        try
        {
            router.ManualConnections.Connect(endpoint);
        }
        catch
        {
            router.PeerRoutingIds.Remove(endpoint);
            throw;
        }
    }

    public void Disconnect(string endpoint)
    {
        ValidateEndpoint(endpoint);
        router.ManualConnections.Disconnect(endpoint);
        router.PeerRoutingIds.Remove(endpoint);
    }

    public IReadOnlyList<ZLinkMeshPeerConnection> ListConnections()
    {
        return router.ManualConnections.ListConnections()
            .Select(endpoint => new ZLinkMeshPeerConnection(
                endpoint,
                router.PeerRoutingIds.TryGetValue(endpoint, out var rid) ? rid : null))
            .ToArray();
    }

    private static void ValidateEndpoint(string endpoint)
    {
        if (string.IsNullOrWhiteSpace(endpoint))
            throw new ZLinkConfigurationException("Manual MeshNode peer endpoint must not be empty.");
    }
}
