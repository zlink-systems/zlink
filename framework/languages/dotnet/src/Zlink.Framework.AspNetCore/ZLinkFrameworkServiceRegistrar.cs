using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.DependencyInjection.Extensions;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;

namespace Zlink.Framework.AspNetCore;

internal static class ZLinkFrameworkServiceRegistrar
{
    public static IServiceCollection AddFrameworkRuntime(
        IServiceCollection services,
        ZLinkFrameworkRegistration registration)
    {
        return services
            .AddScannedHandlers(registration)
            .AddCoreRuntime(registration)
            .AddPublicClients(registration)
            .AddApplicationServices(registration)
            .AddLocationRuntime(registration);
    }

    private static IServiceCollection AddScannedHandlers(
        this IServiceCollection services,
        ZLinkFrameworkRegistration registration)
    {
        foreach (var endpoint in registration.ScannedHandlerCatalog.ChannelEndpoints)
        {
            services.TryAddTransient(endpoint.DeclaringType);
            services.AddSingleton(endpoint);
        }

        foreach (var channel in registration.Channels.Values) AddExplicitChannelHandlers(services, channel);

        return services;
    }

    private static void AddExplicitChannelHandlers(
        IServiceCollection services,
        ZLinkChannelRegistration channel)
    {
        foreach (var handler in channel.SendHandlers)
            AddExplicitChannelHandler(
                services,
                channel.ChannelName,
                handler,
                typeof(IZLinkSendHandler<>).MakeGenericType(handler.MessageType),
                ZLinkMessageKind.Command);

        foreach (var handler in channel.RequestHandlers)
            AddExplicitChannelHandler(
                services,
                channel.ChannelName,
                handler,
                typeof(IZLinkRequestHandler<,>).MakeGenericType(handler.MessageType, handler.ReplyType!),
                ZLinkMessageKind.Request);

        foreach (var handler in channel.PublishHandlers)
            AddExplicitChannelHandler(
                services,
                channel.ChannelName,
                handler,
                typeof(IZLinkFanoutHandler<>).MakeGenericType(handler.MessageType),
                ZLinkMessageKind.Publish);
    }

    private static void AddExplicitChannelHandler(
        IServiceCollection services,
        string channelName,
        ZLinkChannelHandlerRegistration handler,
        Type handlerInterface,
        ZLinkMessageKind kind)
    {
        services.TryAddTransient(handler.HandlerType);
        services.AddSingleton(ZLinkHandlerScanner.CreateExplicitInterfaceDescriptor(
            handler.HandlerType,
            handlerInterface,
            kind,
            channelName,
            handler.PacketName));
    }

    private static IServiceCollection AddCoreRuntime(
        this IServiceCollection services,
        ZLinkFrameworkRegistration registration)
    {
        services.AddSingleton(registration);
        services.TryAddSingleton<IZLinkBackendAdapterFactory, ZLinkDotNetBackendAdapterFactory>();

        // Framework-internal consumer for cross-node bound-session pushes
        // (spec 31 §6); the node route dispatcher resolves it from DI when a
        // relayed push arrives on a router-capable node.
        services.TryAddScoped<ZLinkRemoteSessionBindRouteHandler>();
        services.TryAddScoped<ZLinkRemoteSessionUnbindRouteHandler>();
        services.TryAddScoped<ZLinkRemoteSessionOwnerTombstoneRouteHandler>();
        services.TryAddScoped<ZLinkRemoteSessionPushRelayHandler>();
        services.TryAddScoped<ZLinkRemoteActorFrameRelayHandler>();
        services.TryAddScoped<ZLinkRemoteActorReplyRelayHandler>();
        services.TryAddScoped<ZLinkSessionRouteSealHandler>();
        services.TryAddScoped<ZLinkSessionRouteAbortHandler>();
        services.TryAddScoped<ZLinkSessionRouteCommitHandler>();
        services.TryAddScoped<ZLinkSessionRouteUnsealHandler>();

        registration.DispatchOptions.Diagnostics.LiveLevel ??=
            new ZLinkDiagnosticsLevelCell(
                registration.DispatchOptions.Diagnostics.ConfiguredLevel);
        ZLinkTelemetry.SetDiagnosticsLevel(
            registration.DispatchOptions.Diagnostics.EffectiveLevel);
        services.TryAddSingleton(static provider =>
            new ZLinkHandlerRegistry(
                provider.GetServices<ZLinkHandlerEndpointDescriptor>()));
        services.TryAddSingleton<ZLinkHandlerDispatcher>();
        services.TryAddSingleton<ZLinkDrainAdmissionGate>();
        services.TryAddSingleton<ZLinkFrameworkHostLifecycleState>();
        services.AddSingleton(static provider => new ZLinkAutoConnectLifecycleCoordinator(
            provider.GetService<ZLinkLocationAutoConnectHost>()));
        services.AddSingleton(static provider =>
            new ZLinkFrameworkRuntime(
                provider,
                provider.GetRequiredService<IZLinkBackendAdapterFactory>(),
                provider.GetRequiredService<ZLinkFrameworkRegistration>(),
                provider.GetRequiredService<ZLinkHandlerRegistry>(),
                provider.GetRequiredService<ZLinkHandlerDispatcher>()));
        services.TryAddSingleton<ZLinkSpotRetireTargetRuntime>();
        services.TryAddSingleton<IZLinkSpotRetireTarget>(static provider =>
            provider.GetRequiredService<ZLinkSpotRetireTargetRuntime>());
        services.AddSingleton<IZLinkMessageMetadataPolicy, ZLinkMessageMetadataPolicy>();
        services.TryAddSingleton<IZLinkDrainExecutor>(provider =>
            new ZLinkFrameworkDrainExecutor(
                provider.GetRequiredService<ZLinkFrameworkRuntime>(),
                provider.GetRequiredService<ZLinkRouteMeshRuntimeService>(),
                registration.Locations.Options,
                provider.GetService<ZLinkLocationAutoConnectHost>(),
                provider.GetService<ZLinkLocationRuntime>(),
                provider.GetService<ILogger<ZLinkFrameworkDrainExecutor>>()));
        services.TryAddSingleton<ZLinkDrainCoordinator>(static provider =>
            new ZLinkDrainCoordinator(
                provider.GetRequiredService<ZLinkDrainAdmissionGate>(),
                provider.GetRequiredService<IZLinkDrainExecutor>(),
                flowCaptureEnabled: () =>
                    provider.GetRequiredService<ZLinkFrameworkRuntime>().Flow.CaptureEnabled,
                logger: provider.GetService<ILogger<ZLinkDrainCoordinator>>()));
        services.TryAddSingleton<ZLinkFrameworkMaintenanceRuntime>(provider =>
            new ZLinkFrameworkMaintenanceRuntime(
                provider.GetRequiredService<ZLinkDrainCoordinator>(),
                provider.GetRequiredService<ZLinkFrameworkHostLifecycleState>(),
                provider.GetRequiredService<ZLinkFrameworkRuntime>().PreflightRetireAsync,
                provider.GetRequiredService<ZLinkFrameworkRuntime>().PublishRetiringAsync,
                registration.ApplicationVersion,
                inboundDispatchSnapshot: provider.GetRequiredService<
                    ZLinkFrameworkRuntime>().SnapshotInboundDispatch,
                acceptingWorkSnapshot: () => provider.GetRequiredService<
                    ZLinkFrameworkRuntime>().IsAcceptingApplicationWork,
                logger: provider.GetService<ILogger<ZLinkFrameworkMaintenanceRuntime>>()));
        services.TryAddSingleton<IZLinkFrameworkRuntime>(static provider =>
            provider.GetRequiredService<ZLinkFrameworkMaintenanceRuntime>());
        services.TryAddSingleton<IZLinkRuntimeTerminalFailureSink>(static provider =>
            provider.GetRequiredService<ZLinkFrameworkMaintenanceRuntime>());
        services.AddSingleton<IHostedService>(static provider =>
            new ZLinkFrameworkHostedService(
                provider.GetRequiredService<ZLinkFrameworkRuntime>(),
                provider.GetRequiredService<ZLinkRouteMeshRuntimeService>(),
                provider.GetService<ZLinkLocationRuntime>(),
                provider.GetRequiredService<ZLinkAutoConnectLifecycleCoordinator>(),
                provider.GetService<ZLinkLocationLifecycle>(),
                provider.GetRequiredService<ZLinkFrameworkMaintenanceRuntime>()));

        return services;
    }

    private static IServiceCollection AddPublicClients(
        this IServiceCollection services,
        ZLinkFrameworkRegistration registration)
    {
        services.AddSingleton<IZLinkRouteMeshRuntimeOptions>(static provider =>
            new ZLinkRouteMeshRuntimeOptionsService(
                provider.GetRequiredService<ZLinkFrameworkRuntime>()));
        services.AddSingleton(static provider =>
            new ZLinkRouteMeshRuntimeService(
                provider.GetRequiredService<ZLinkFrameworkRuntime>(),
                provider.GetRequiredService<ZLinkFrameworkHostLifecycleState>(),
                provider.GetService<ZLinkLocationStoreHealth>(),
                provider.GetService<IZLinkLocationDescriptorQuery>()));
        services.AddSingleton<IZLinkRouteMeshRuntime>(static provider =>
            provider.GetRequiredService<ZLinkRouteMeshRuntimeService>());
        services.AddSingleton(static provider =>
            new ZLinkClientServerRuntimeService(
                provider.GetRequiredService<ZLinkFrameworkRuntime>(),
                provider.GetRequiredService<ZLinkFrameworkHostLifecycleState>(),
                provider.GetService<ZLinkLocationStoreHealth>()));
        services.AddSingleton<IZLinkClientServerRuntime>(static provider =>
            provider.GetRequiredService<ZLinkClientServerRuntimeService>());
        services.AddSingleton(static provider =>
            new ZLinkDiagnosticsRuntimeService(
                provider.GetRequiredService<ZLinkFrameworkRegistration>()
                    .DispatchOptions.Diagnostics));
        services.AddSingleton<IZLinkDiagnosticsRuntime>(static provider =>
            provider.GetRequiredService<ZLinkDiagnosticsRuntimeService>());
        services.AddSingleton<ZLinkRouteClient>();
        services.AddSingleton<IZLinkRouteClient>(static provider => provider.GetRequiredService<ZLinkRouteClient>());
        services.AddSingleton<IZLinkSpotClient, ZLinkSpotClient>();
        services.AddSingleton<ZLinkFanoutClient>();
        services.AddSingleton<IZLinkFanoutClient>(static provider => provider.GetRequiredService<ZLinkFanoutClient>());
        services.AddSingleton(static provider =>
            new ZLinkFanoutRuntimeService(
                provider.GetRequiredService<ZLinkFrameworkRegistration>(),
                provider.GetRequiredService<ZLinkFrameworkHostLifecycleState>()));
        services.AddSingleton<IZLinkFanoutRuntime>(static provider =>
            provider.GetRequiredService<ZLinkFanoutRuntimeService>());

        if (HasSpotNode(registration))
        {
            services.AddSingleton<IZLinkSpotManager>(static provider =>
                provider.GetRequiredService<ZLinkFrameworkRuntime>());
            services.AddSingleton<IZLinkSpotOutbound, ZLinkSpotOutboundService>();
            services.AddSingleton<ZLinkSpotPublisherClientService>();
            services.AddSingleton<IZLinkSpotPublisherClient>(static provider =>
                provider.GetRequiredService<ZLinkSpotPublisherClientService>());
        }

        if (HasActorCapableSpotNode(registration))
        {
            services.AddSingleton<ZLinkActorManagerService>();
            services.AddSingleton<IZLinkActorManager>(static provider =>
                provider.GetRequiredService<ZLinkActorManagerService>());
        }

        if (HasActorCapableSpotNode(registration) || registration.Locations.Enabled)
        {
            services.AddSingleton(static provider =>
                new ZLinkActorDirectory(
                    provider.GetRequiredService<ZLinkFrameworkRuntime>(),
                    provider.GetRequiredService<ZLinkFrameworkRegistration>(),
                    provider.GetService<ZLinkStoreLocationResolvers>()));
            services.AddSingleton<IZLinkActorResolver>(static provider =>
                provider.GetRequiredService<ZLinkActorDirectory>());
        }

        if (HasRouterSpotNode(registration) && registration.Locations.Enabled)
        {
            services.AddSingleton<ZLinkActorClient>();
            services.AddSingleton<IZLinkActorClient>(static provider =>
                provider.GetRequiredService<ZLinkActorClient>());
        }

        return services;
    }

    private static IServiceCollection AddApplicationServices(
        this IServiceCollection services,
        ZLinkFrameworkRegistration registration)
    {
        foreach (var filterType in registration.Filters) services.AddTransient(filterType);

        foreach (var actorFactoryType in registration.ActorCatalog.Factories.Values)
            services.TryAddScoped(actorFactoryType);

        foreach (var adapterType in registration.SpotNodes.Values
                     .SelectMany(static node =>
                         node.SpotRelocations.Values
                             .Concat(node.InstanceSpotRelocations.Values)
                             .Concat(node.ActorRelocations.Values))
                     .Select(static relocation => relocation.AdapterType)
                     .OfType<Type>()
                     .Distinct())
            services.TryAddScoped(adapterType);

        foreach (var spotType in registration.SpotNodes.Values
                     .SelectMany(static spotNode => spotNode.SpotFactories))
            services.TryAddScoped(spotType);

        foreach (var entrySpotType in registration.SpotNodes.Values
                     .Select(static spotNode => spotNode.EntrySpotType)
                     .OfType<Type>())
            services.TryAddScoped(entrySpotType);

        foreach (var membership in registration.SpotNodes.Values
                     .SelectMany(static spotNode => spotNode.ChannelMemberships))
        {
            foreach (var handler in membership.SendHandlers)
                services.TryAddScoped(handler.HandlerType);

            foreach (var handler in membership.RequestHandlers)
                services.TryAddScoped(handler.HandlerType);
        }

        foreach (var meshNode in registration.SpotNodes.Values)
        {
            foreach (var handler in meshNode.RouteSendHandlers)
                services.TryAddScoped(handler.HandlerType);

            foreach (var handler in meshNode.RouteRequestHandlers)
                services.TryAddScoped(handler.HandlerType);
        }

        return services;
    }

    private static bool HasSpotNode(ZLinkFrameworkRegistration registration)
    {
        return registration.SpotNodes.Count > 0;
    }

    private static bool HasRouterSpotNode(ZLinkFrameworkRegistration registration)
    {
        return registration.SpotNodes.Values.Any(static spotNode => spotNode.Router is not null);
    }

    private static bool HasActorCapableSpotNode(ZLinkFrameworkRegistration registration)
    {
        return registration.SpotNodes.Values.Any(static spotNode =>
            spotNode.Router is not null
            && spotNode.ObjectRole is ZLinkMeshNodeObjectRole.Client
                or ZLinkMeshNodeObjectRole.Server);
    }
    private static IServiceCollection AddLocationRuntime(
        this IServiceCollection services,
        ZLinkFrameworkRegistration registration)
    {
        var locations = registration.Locations;
        if (!locations.Enabled) return services;

        services.AddSingleton(locations.Options);
        if (locations.StoreInstance is { } store)
        {
            services.AddSingleton(new ZLinkLocationStoreInstanceOwner(
                store,
                locations.RelocationStoreInstance));
            services.AddSingleton<IHostedService>(static provider =>
                provider.GetRequiredService<ZLinkLocationStoreInstanceOwner>());
            services.AddSingleton<IZLinkLocationStore>(store);
            if (locations.RelocationStoreInstance is { } relocationStore)
                services.AddSingleton<IZLinkRelocationStore>(relocationStore);
            services.AddSingleton(locations.ResolveStore()!);
        }
        else if (locations.UseInMemoryStores)
        {
            services.AddSingleton(
                (ZLinkInMemoryLocationStore)locations.ResolveStore()!);
            services.AddSingleton<IZLinkLocationRepository>(
                static provider => provider.GetRequiredService<ZLinkInMemoryLocationStore>());
        }
        else if (locations.ResolveStore() is { } repository)
        {
            services.AddSingleton(repository);
        }
        else
        {
            return services;
        }

        services.AddSingleton<ZLinkLocationStoreHealth>();
        services.AddSingleton(static provider => new ZLinkOwnerLeaseTracker(
            provider.GetRequiredService<IZLinkLocationRepository>(),
            provider.GetRequiredService<ZLinkLocationOptions>(),
            health: provider.GetRequiredService<ZLinkLocationStoreHealth>()));
        // One observed-generation guard per runtime, shared by every read
        // surface, so no read path ever rolls the view backwards.
        services.AddSingleton<ZLinkObservedLocationGenerations>();
        services.AddSingleton(static provider => new ZLinkStoreLocationResolvers(
            provider.GetRequiredService<IZLinkLocationRepository>(),
            provider.GetRequiredService<ZLinkOwnerLeaseTracker>(),
            provider.GetRequiredService<ZLinkObservedLocationGenerations>(),
            health: provider.GetRequiredService<ZLinkLocationStoreHealth>(),
            options: provider.GetRequiredService<ZLinkLocationOptions>()));
        services.AddSingleton<IZLinkMeshNodeLocationResolver>(
            static provider => provider.GetRequiredService<ZLinkStoreLocationResolvers>());
        services.AddSingleton(provider => new ZLinkSpotMeshLocationResolver(
            registration,
            provider.GetRequiredService<ZLinkStoreLocationResolvers>()));
        services.AddSingleton(static provider => new ZLinkLocationAddressResolvers(
            provider.GetRequiredService<ZLinkStoreLocationResolvers>()));
        services.AddSingleton(static provider => new ZLinkSpotHandleWatchHost(
            provider.GetService<IZLinkLocationWatchStore>(),
            provider.GetRequiredService<ZLinkStoreLocationResolvers>(),
            handles: null,
            options: provider.GetRequiredService<ZLinkLocationOptions>()));
        services.AddSingleton<IHostedService>(static provider =>
            provider.GetRequiredService<ZLinkSpotHandleWatchHost>());
        services.AddSingleton(provider => new ZLinkLocationRuntime(
            provider.GetRequiredService<ZLinkLocationOptions>(),
            provider.GetRequiredService<IZLinkLocationRepository>(),
            observed: provider.GetRequiredService<ZLinkObservedLocationGenerations>(),
            metricScopes: registration.SpotNodes.Keys
                .Select(static name => new KeyValuePair<string, string>("mesh", name))
                .Concat(registration.Channels.Keys.Select(static name =>
                    new KeyValuePair<string, string>("channel", name)))
                .Distinct()
                .ToArray()));
        // Every mesh namespace this host can advertise or dial under; the
        // operational query enumerates these when no mesh filter is given.
        var registeredMeshNames = registration.Channels.Values
            .Select(static channel => channel.ChannelName)
            .Concat(registration.SpotNodes.Values.Select(static spot =>
                spot.SpotMeshChannelName ?? spot.SpotNodeName))
            .Distinct(StringComparer.Ordinal)
            .ToArray();
        services.AddSingleton(
            provider => new ZLinkLocationRuntimeQueryService(
                provider.GetRequiredService<ZLinkLocationOptions>(),
                provider.GetRequiredService<IZLinkLocationRepository>(),
                registeredMeshNames,
                provider.GetRequiredService<ZLinkOwnerLeaseTracker>(),
                provider.GetRequiredService<ZLinkLocationRuntime>(),
                provider.GetRequiredService<ZLinkObservedLocationGenerations>(),
                provider.GetRequiredService<ZLinkLocationStoreHealth>()));
        services.AddSingleton<IZLinkLocationRuntimeQuery>(static provider =>
            provider.GetRequiredService<ZLinkLocationRuntimeQueryService>());
        services.AddSingleton<IZLinkLocationDescriptorQuery>(static provider =>
            provider.GetRequiredService<ZLinkLocationRuntimeQueryService>());
        services.AddSingleton<IZLinkLocationReadiness>(
            static provider => new ZLinkLocationReadiness(
                provider.GetRequiredService<IZLinkLocationRuntimeQuery>()));
        services.AddSingleton(static provider => new ZLinkLocationLifecycle(
            provider.GetRequiredService<ZLinkLocationRuntime>(),
            provider.GetRequiredService<ZLinkStoreLocationResolvers>()));
        services.AddSingleton<IZLinkActorLocationLifecycle>(
            static provider => provider.GetRequiredService<ZLinkLocationLifecycle>().ActorOwnership);
        services.AddSingleton(static provider =>
        {
            var owner = provider.GetService<ZLinkLocationStoreInstanceOwner>();
            var host = new ZLinkLocationAutoConnectHost(
                provider.GetRequiredService<ZLinkLocationRuntime>(),
                provider.GetRequiredService<IZLinkMeshNodeLocationResolver>(),
                provider.GetRequiredService<ZLinkLocationOptions>(),
                provider.GetService<IZLinkLocationWatchStore>(),
                leaseTracker: provider.GetRequiredService<ZLinkOwnerLeaseTracker>(),
                store: provider.GetRequiredService<IZLinkLocationRepository>());
            // The store owner enforces this dependency even when host startup
            // fails and the DI container starts disposing services concurrently.
            owner?.RegisterBeforeStoreDispose(host);
            return host;
        });
        services.AddSingleton<IZLinkAutoConnectTopologyQuery>(static provider =>
            provider.GetRequiredService<ZLinkLocationAutoConnectHost>());
        return services;
    }

}

internal sealed class ZLinkLocationStoreInstanceOwner(
    IZLinkLocationStore store,
    IZLinkRelocationStore? relocationStore = null)
    : IHostedService, IAsyncDisposable
{
    private readonly object _disposeGate = new();
    private readonly List<IAsyncDisposable> _beforeStoreDispose = [];
    private Task? _disposeTask;

    public IZLinkLocationStore Store { get; } = store;

    private IReadOnlyList<object> Stores { get; } =
        relocationStore is null || ReferenceEquals(store, relocationStore)
            ? [store]
            : [store, relocationStore];

    public Task StartAsync(CancellationToken cancellationToken) => Task.CompletedTask;

    public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;

    internal void RegisterBeforeStoreDispose(IAsyncDisposable dependent)
    {
        ArgumentNullException.ThrowIfNull(dependent);
        lock (_disposeGate)
        {
            ObjectDisposedException.ThrowIf(_disposeTask is not null, this);
            _beforeStoreDispose.Add(dependent);
        }
    }

    public ValueTask DisposeAsync()
    {
        Task task;
        TaskCompletionSource? start = null;
        lock (_disposeGate)
        {
            if (_disposeTask is null)
            {
                start = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                _disposeTask = DisposeCoreAsync(start.Task);
            }
            task = _disposeTask;
        }
        start?.TrySetResult();
        return new ValueTask(task);
    }

    private async Task DisposeCoreAsync(Task started)
    {
        await started.ConfigureAwait(false);
        List<Exception>? failures = null;
        foreach (var dependent in _beforeStoreDispose)
        {
            try
            {
                await dependent.DisposeAsync().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }

        foreach (var storeInstance in Stores)
        {
            try
            {
                if (storeInstance is IAsyncDisposable asyncDisposable)
                    await asyncDisposable.DisposeAsync().ConfigureAwait(false);
                else if (storeInstance is IDisposable disposable)
                    disposable.Dispose();
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }

        if (failures is { Count: 1 })
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures is { Count: > 1 }) throw new AggregateException(failures);
    }
}
