using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace Zlink.Framework.Runtime.Host;

internal sealed record ZLinkFrameworkRuntimeComponents(
    ZLinkChannelRuntimeManager Channels,
    ZLinkStreamRuntimeManager Streams,
    ZLinkSpotRuntimeManager Spots,
    ZLinkFrameworkComponentStateFactory StateFactory,
    ZLinkActorSessionManager ActorSessionManager,
    ZLinkFrameworkActorFacade Actors);

internal static class ZLinkFrameworkRuntimeComponentFactory
{
    public static ZLinkFrameworkRuntimeComponents Create(
        ZLinkFrameworkRuntime runtime,
        IServiceProvider services,
        IZLinkBackendAdapterFactory backendAdapterFactory,
        ZLinkFrameworkRegistration registration,
        ZLinkLocationLifecycle? locationLifecycle,
        ZLinkHandlerRegistry handlerRegistry,
        ZLinkHandlerDispatcher dispatcher,
        Func<ZLinkFrameworkComponentState> getOrStartState,
        Func<IZLinkBackendSpotNode?> getActorSpotNode,
        Func<string, ZLinkActivationConcurrencyAdmission?>? getActivationAdmission = null)
    {
        var loggerFactory = services.GetService<ILoggerFactory>();
        var channelLogger = loggerFactory?.CreateLogger("Zlink.Framework.ClientServer")
                            ?? NullLogger.Instance;
        var channelDispatchErrors = new ZLinkDispatchErrorReporter(
            registration.DispatchOptions,
            ZLinkMessageFlowTracer.CreateLogger(loggerFactory, channelLogger),
            runtime);
        IReadOnlySet<string> ResolveHandlerGroups(string channelName) =>
            registration.Channels.TryGetValue(channelName, out var channel)
                ? channel.HandlerGroups
                : new HashSet<string>(StringComparer.Ordinal);

        var channels = new ZLinkChannelRuntimeManager(
            backendAdapterFactory,
            registration,
            new ZLinkChannelReceiveLoop(
                new ZLinkFanoutPacketDispatcher(
                    handlerRegistry,
                    dispatcher,
                    registration,
                    runtime,
                    loggerFactory?.CreateLogger<ZLinkFanoutPacketDispatcher>()),
                new ZLinkClientServerDispatcher(
                    new ZLinkChannelCommandDispatchPipeline(
                        null,
                        handlerRegistry,
                        dispatcher,
                        ResolveHandlerGroups,
                        LogLevel.Warning,
                        channelDispatchErrors,
                        registration.Codecs,
                        channelLogger),
                    new ZLinkChannelRequestDispatchPipeline(
                        null,
                        handlerRegistry,
                        dispatcher,
                        ResolveHandlerGroups,
                        registration.Codecs,
                        channelDispatchErrors,
                        channelLogger),
                    registration.Codecs)),
            services.GetService<ZLinkFanoutRuntimeService>()
            ?? new ZLinkFanoutRuntimeService(registration));
        var streams = new ZLinkStreamRuntimeManager(services, backendAdapterFactory, registration);
        var spots = new ZLinkSpotRuntimeManager(
            services,
            runtime,
            backendAdapterFactory,
            registration,
            locationLifecycle);
        var stateFactory = new ZLinkFrameworkComponentStateFactory(
            runtime,
            backendAdapterFactory,
            registration,
            channels,
            streams,
            spots);
        var actorSessionManager = new ZLinkActorSessionManager(
            runtime,
            services,
            getActorSpotNode,
            locationLifecycle,
            new ZLinkBoundSessionService(runtime),
            getActivationAdmission);
        var actors = new ZLinkFrameworkActorFacade(
            runtime,
            registration,
            services,
            spots,
            actorSessionManager,
            getOrStartState,
            getActorSpotNode);
        return new ZLinkFrameworkRuntimeComponents(
            channels,
            streams,
            spots,
            stateFactory,
            actorSessionManager,
            actors);
    }
}
