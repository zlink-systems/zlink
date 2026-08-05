using System.Reflection;

namespace Zlink.Framework.Runtime.Configuration.Builders;

internal sealed class ZLinkFrameworkOptionsBuilder : IZLinkFrameworkOptions
{
    private readonly ZLinkFrameworkRegistration _registration;

    public ZLinkFrameworkOptionsBuilder(ZLinkFrameworkRegistration registration)
    {
        _registration = registration;
    }

    public TimeSpan DefaultRequestTimeout
    {
        get => _registration.DefaultRequestTimeout;
        set
        {
            ZLinkRequestTimeoutValidation.Validate(value, nameof(DefaultRequestTimeout));
            _registration.DefaultRequestTimeout = value;
        }
    }

    public TimeSpan DefaultSocketSendTimeout
    {
        get => _registration.DefaultSocketSendTimeout;
        set
        {
            _registration.DefaultSocketSendTimeout =
                ZLinkSocketConfig.NormalizeSendTimeout(value)
                ?? throw new ZLinkConfigurationException("DefaultSocketSendTimeout is required.");
        }
    }

    public long ApplicationVersion
    {
        get => _registration.ApplicationVersion;
        set
        {
            if (value < 0)
                throw new ZLinkConfigurationException(
                    "ApplicationVersion must not be negative.");
            _registration.ApplicationVersion = value;
        }
    }

    public string? MaintenanceWave
    {
        get => _registration.MaintenanceWave;
        set
        {
            if (value is not null)
            {
                var size = System.Text.Encoding.UTF8.GetByteCount(value);
                if (size is < 1 or > 255 || value.Contains('\0'))
                    throw new ZLinkConfigurationException(
                        "MaintenanceWave must be 1 to 255 UTF-8 bytes without NUL.");
            }
            _registration.MaintenanceWave = value;
        }
    }

    public IZLinkCodecRegistryBuilder Codecs => _registration.Codecs;

    public IZLinkWorkerOptions Worker => _registration.WorkerOptions;

    public void AddHandlersFromAssemblyOf<TMarker>()
    {
        AddHandlersFromAssembly(typeof(TMarker).Assembly);
    }

    public void AddHandlersFromAssemblyOf(Type markerType)
    {
        ArgumentNullException.ThrowIfNull(markerType);

        AddHandlersFromAssembly(markerType.Assembly);
    }

    public void AddHandlersFromAssembly(Assembly assembly)
    {
        ArgumentNullException.ThrowIfNull(assembly);

        _registration.HandlerAssemblies.Add(assembly);
    }

    public void DisableImplicitHandlerAutoRegistration()
    {
        _registration.ImplicitHandlerAutoRegistrationEnabled = false;
    }

    public IZLinkMetadataPolicyBuilder ConfigureMetadata()
    {
        return new ZLinkMetadataPolicyBuilder(_registration.MetadataPolicy);
    }

    public IZLinkFanoutChannelBuilder AddFanoutChannel(string channelName)
    {
        var channel = AddChannelRegistration(channelName, ZLinkLocationAutoConnectType.Fanout);
        return new ZLinkFanoutChannelBuilder(channel);
    }

    public IZLinkClientServerChannelRoleBuilder AddClientServerChannel(
        string channelName)
    {
        if (string.IsNullOrWhiteSpace(channelName))
            throw new ZLinkConfigurationException("Channel name must not be empty.");

        if (!_registration.Channels.TryGetValue(channelName, out var channel))
        {
            channel = new ZLinkChannelRegistration
            {
                ChannelName = channelName,
                AutoConnectType = ZLinkLocationAutoConnectType.ClientServer
            };
            _registration.Channels.Add(channelName, channel);
        }
        else if (channel.AutoConnectType != ZLinkLocationAutoConnectType.ClientServer)
        {
            throw new ZLinkConfigurationException(
                $"Duplicate channel name '{channelName}'.");
        }

        return new ZLinkClientServerChannelRoleBuilder(channel);
    }

    // Test-only in-memory location store registration (spec 05-route-mesh §7 /
    // gap 90 §12.33 S8-08). Not on IZLinkFrameworkOptions; single-process contract
    // tests reach it through the internal ZLinkTestLocationStores helper. Production
    // hosts register a distributed store via AddLocationStore or fail fast.
    internal void UseTestLocationStore()
    {
        _registration.Locations.UseInMemoryStores = true;
    }

    public void AddLocationStore(IZLinkLocationStore store)
    {
        ArgumentNullException.ThrowIfNull(store);

        if (_registration.Locations.StoreInstance is not null)
            throw new ZLinkConfigurationException(
                "A Location Store is already registered.");
        _registration.Locations.StoreInstance = store;
    }

    public void AddRelocationStore(IZLinkRelocationStore store)
    {
        ArgumentNullException.ThrowIfNull(store);

        if (_registration.Locations.RelocationStoreInstance is not null)
            throw new ZLinkConfigurationException(
                "A Relocation Store is already registered.");
        _registration.Locations.RelocationStoreInstance = store;
    }

    internal void AddLocationRepositoryForTests(
        IZLinkLocationRepository repository)
    {
        ArgumentNullException.ThrowIfNull(repository);
        _registration.Locations.SetTestRepository(repository);
    }

    internal void AddRelocationRepositoryForTests(
        IZLinkRelocationRepository repository)
    {
        ArgumentNullException.ThrowIfNull(repository);
        _registration.Locations.SetTestRelocationRepository(repository);
    }

    public ZLinkLocationOptions ConfigureLocations()
    {
        return _registration.Locations.Options;
    }

    public IZLinkInboundDispatchOptions ConfigureInboundDispatch()
    {
        return _registration.InboundDispatchOptions;
    }

    public IZLinkNetworkOptions ConfigureNetwork()
    {
        return _registration.NetworkOptions;
    }

    public void UseFilter<TFilter>()
        where TFilter : class, IZLinkHandlerFilter
    {
        _registration.Filters.Add(typeof(TFilter));
    }

    public IZLinkDispatchOptions ConfigureDispatch()
    {
        return _registration.DispatchOptions;
    }

    public IZLinkStreamCompressionBuilder ConfigureStreamCompression()
    {
        return new ZLinkStreamCompressionBuilder(_registration);
    }

    public IZLinkStreamNodeBuilder AddStreamNode(string streamNodeName)
    {
        var streamNode = ZLinkRegistrationBuilderGuard.AddUnique(
            _registration.StreamNodes,
            streamNodeName,
            () => new ZLinkStreamNodeRegistration { StreamNodeName = streamNodeName },
            "STREAM node name must not be empty.",
            $"Duplicate stream node name '{streamNodeName}'.");

        return new ZLinkStreamNodeBuilder(streamNode);
    }

    public IZLinkMeshNodeBuilder AddRouteMesh(string meshName)
    {
        if (string.IsNullOrWhiteSpace(meshName))
            throw new ZLinkConfigurationException("RouteMesh name must not be empty.");
        if (_registration.SpotNodes.ContainsKey(meshName))
            throw new ZLinkConfigurationException(
                $"Duplicate RouteMesh name '{meshName}'.");

        var meshNode = ZLinkRegistrationBuilderGuard.RegisterSpotNode(
            _registration.SpotNodes,
            meshName);
        meshNode.SpotMeshChannelName = meshName;

        return new ZLinkMeshNodeBuilder(meshNode);
    }

    private ZLinkChannelRegistration AddChannelRegistration(
        string channelName,
        ZLinkLocationAutoConnectType autoConnectType)
    {
        return ZLinkRegistrationBuilderGuard.AddUnique(
            _registration.Channels,
            channelName,
            () => new ZLinkChannelRegistration
            {
                ChannelName = channelName,
                AutoConnectType = autoConnectType
            },
            "Channel name must not be empty.",
            $"Duplicate channel name '{channelName}'.");
    }

}

internal static class ZLinkRegistrationBuilderGuard
{
    public static ZLinkSpotNodeRegistration RegisterSpotNode(
        Dictionary<string, ZLinkSpotNodeRegistration> registrations,
        string spotNodeName)
    {
        return AddUnique(
            registrations,
            spotNodeName,
            () => new ZLinkSpotNodeRegistration { SpotNodeName = spotNodeName },
            "SPOT node name must not be empty.",
            $"Duplicate spot node name '{spotNodeName}'.");
    }

    public static void AddUnique<TValue>(
        Dictionary<string, TValue> registrations,
        string name,
        TValue value,
        string emptyMessage,
        string duplicateMessage)
    {
        AddUnique(registrations, name, () => value, emptyMessage, duplicateMessage);
    }

    public static TValue AddUnique<TValue>(
        Dictionary<string, TValue> registrations,
        string name,
        Func<TValue> create,
        string emptyMessage,
        string duplicateMessage)
    {
        if (string.IsNullOrWhiteSpace(name)) throw new ZLinkConfigurationException(emptyMessage);

        var value = create();
        if (!registrations.TryAdd(name, value)) throw new ZLinkConfigurationException(duplicateMessage);

        return value;
    }
}

internal sealed class ZLinkMetadataPolicyBuilder(ZLinkMetadataPolicyRegistration registration)
    : IZLinkMetadataPolicyBuilder
{
    public IZLinkMetadataPolicyBuilder AllowSessionToActor(string key)
    {
        AddKey(registration.SessionToActorKeys, key);
        return this;
    }

    public IZLinkMetadataPolicyBuilder AllowActorToSession(string key)
    {
        AddKey(registration.ActorToSessionKeys, key);
        return this;
    }

    private static void AddKey(HashSet<string> keys, string key)
    {
        if (string.IsNullOrWhiteSpace(key))
            throw new ZLinkConfigurationException("Metadata key must not be empty.");

        keys.Add(key);
    }
}
