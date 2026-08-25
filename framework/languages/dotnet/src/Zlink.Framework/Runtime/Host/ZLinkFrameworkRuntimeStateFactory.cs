namespace Zlink.Framework.Runtime.Host;

internal sealed class ZLinkFrameworkComponentStateFactory(
    ZLinkFrameworkRuntime frameworkRuntime,
    IZLinkBackendAdapterFactory backendAdapterFactory,
    ZLinkFrameworkRegistration registration,
    ZLinkChannelRuntimeManager channels,
    ZLinkStreamRuntimeManager streams,
    ZLinkSpotRuntimeManager spots)
{
    public async ValueTask<ZLinkFrameworkComponentState> CreateAsync()
    {
        var effectiveProcessorCount =
            ZLinkApplicationJobQueueCapacityResolver.ResolveEffectiveProcessorCount(
                Environment.ProcessorCount,
                registration.WorkerOptions.MaxThreads);
        ZLinkFrameworkRegistrationValidator.ValidateInboundDispatch(
            registration,
            effectiveProcessorCount);
        var applicationJobQueueCapacity =
            ZLinkApplicationJobQueueCapacityResolver.Resolve(
                registration.InboundDispatchOptions.ApplicationJobQueueProfile,
                registration.InboundDispatchOptions.MaxQueuedApplicationJobs,
                effectiveProcessorCount,
                registration.InboundDispatchOptions
                    .ApplicationJobQueuePauseThresholdPercent,
                registration.InboundDispatchOptions
                    .ApplicationJobQueueResumeThresholdPercent);
        await ZLinkSpotStartupValidator.ValidateAsync(
                frameworkRuntime.Services,
                registration)
            .ConfigureAwait(false);

        IZLinkBackendRuntimeContext? context = null;
        ZLinkFrameworkComponentState? state = null;

        try
        {
            context = backendAdapterFactory.CreateRuntimeContext();
            context.ConfigureCoreHwm(
                ToBindingProfile(
                    registration.InboundDispatchOptions.CoreHwmProfile),
                registration.InboundDispatchOptions.CoreHwmMemoryLimitBytes ?? 0,
                registration.InboundDispatchOptions.CoreHwmBudgetBytes ?? 0);
            state = new ZLinkFrameworkComponentState(
                context,
                registration,
                frameworkRuntime.Services,
                frameworkRuntime.PrepareErrorSink(),
                frameworkRuntime.ExecutionOwner,
                applicationJobQueueCapacity);
            await channels.InitializeInboundChannelsAsync(state).ConfigureAwait(false);
            await channels.InitializePublisherChannelsAsync(state).ConfigureAwait(false);
            await spots.InitializeSpotNodesAsync(state).ConfigureAwait(false);
            state.BuildRouteMeshChannelIndex();
            await streams.InitializeStreamNodesAsync(state).ConfigureAwait(false);
            return state;
        }
        catch (Exception error)
        {
            ZLinkFrameworkDebugLog.Startup(error);
            var failures = new ZLinkFailureCollector(error);
            if (state is not null)
            {
                await failures.CaptureAsync(state.DisposeAsync).ConfigureAwait(false);
                frameworkRuntime.DetachErrorSink(state.ErrorSink);
            }
            else if (context is not null)
                await failures.CaptureAsync(context.DisposeAsync).ConfigureAwait(false);
            failures.ThrowIfAny();
            throw new InvalidOperationException("Unreachable after startup cleanup failure propagation.");
        }
    }

    private static AutoHwmProfile ToBindingProfile(
        ZLinkCoreHwmProfile profile) =>
        profile switch
        {
            ZLinkCoreHwmProfile.Compact => AutoHwmProfile.Compact,
            ZLinkCoreHwmProfile.LowLatency => AutoHwmProfile.LowLatency,
            ZLinkCoreHwmProfile.Balanced => AutoHwmProfile.Balanced,
            ZLinkCoreHwmProfile.Throughput => AutoHwmProfile.Throughput,
            _ => throw new ZLinkConfigurationException(
                $"Unknown CoreHwmProfile value '{(int)profile}'.")
        };

}
