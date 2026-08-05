using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Timers;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotActivationFactory(
    IServiceProvider services,
    ZLinkFrameworkRuntime runtime,
    ZLinkFrameworkRegistration frameworkRegistration,
    ZLinkSpotNodeRegistration registration,
    IZLinkBackendSpotNode node,
    string spotChannelName,
    ZLinkCompletionAdmissionOwner completionAdmission,
    ZLinkTimerScheduler timerScheduler)
{
    public async ValueTask<ZLinkSpotActivationCreateResult> CreateAsync(
        Type spotType,
        IZLinkBackendSpot nativeSpot,
        string spotId,
        ZLinkMessage request,
        CancellationToken cancellationToken) =>
        await CreateUserSpotAsync(
                spotType,
                nativeSpot,
                spotId,
                request,
                invokeCreate: true,
                cancellationToken)
            .ConfigureAwait(false);

    internal async ValueTask<ZLinkSpotActivation>
        CreateForRelocationAsync(
            Type spotType,
            IZLinkBackendSpot nativeSpot,
            string spotId,
            CancellationToken cancellationToken) =>
        (await CreateUserSpotAsync(
                spotType,
                nativeSpot,
                spotId,
                ZLinkMessage.Empty,
                invokeCreate: false,
                cancellationToken)
            .ConfigureAwait(false)).Activation;

    private async ValueTask<ZLinkSpotActivationCreateResult>
        CreateUserSpotAsync(
            Type spotType,
            IZLinkBackendSpot nativeSpot,
            string spotId,
            ZLinkMessage request,
            bool invokeCreate,
            CancellationToken cancellationToken)
    {
        AsyncServiceScope spotScope = default;
        var scopeCreated = false;
        ZLinkSpotActivation? activation = null;
        try
        {
            spotScope = services.CreateAsyncScope();
            scopeCreated = true;
            activation = new ZLinkSpotActivation(
                runtime,
                spotScope,
                nativeSpot,
                spotId,
                node.RoutingId,
                registration.SpotNodeName,
                spotChannelName,
                frameworkRegistration.DefaultRequestTimeout,
                registration.Router?.SocketConfig.SendTimeout
                ?? frameworkRegistration.DefaultSocketSendTimeout,
                registration.UserSpotFactoryOptions.TryGetValue(
                    spotType,
                    out var userSpotOptions)
                    ? userSpotOptions.ExecutionMode
                    : ZLinkUserSpotExecutionMode.SpotWide,
                userSpotOptions?.RelocationReadiness
                ?? ZLinkSpotRelocationReadinessMode.AnyTurnBoundary,
                restoreLogicalTimers: !invokeCreate,
                completionAdmission: completionAdmission,
                timerScheduler: timerScheduler);

            var spot = (IZLinkSpot)ActivatorUtilities.CreateInstance(
                spotScope.ServiceProvider,
                spotType,
                activation);

            activation.AttachSpot(spot);
            foreach (var handler in frameworkRegistration.ScannedHandlerCatalog.SpotHandlers)
                await activation.ApplyScannedHandlerAsync(handler, cancellationToken)
                    .ConfigureAwait(false);

            spot.Configure();
            await activation.BindDescriptorsAsync(cancellationToken).ConfigureAwait(false);
            if (!invokeCreate)
                activation.AttachNativeDispatch();
            var response = invokeCreate
                ? await activation.InitializeAsync(request, cancellationToken)
                    .ConfigureAwait(false)
                : ZLinkSpotCreateResponse.Accept();
            return new ZLinkSpotActivationCreateResult(activation, response);
        }
        catch (Exception initializationFailure)
        {
            var failures = new ZLinkFailureCollector(initializationFailure);
            if (activation is null)
            {
                if (scopeCreated)
                    await failures.CaptureAsync(spotScope.DisposeAsync).ConfigureAwait(false);
                await failures.CaptureAsync(nativeSpot.DisposeAsync).ConfigureAwait(false);
            }
            else
            {
                await failures.CaptureAsync(activation.DisposeAsync).ConfigureAwait(false);
            }

            failures.ThrowIfAny();
            throw new InvalidOperationException("Unreachable after startup cleanup failure propagation.");
        }
    }

    public async ValueTask<ZLinkSpotActivation> CreateInstanceAsync(
        Type spotType,
        IZLinkBackendSpot nativeSpot,
        string spotId,
        CancellationToken cancellationToken,
        bool restoreLogicalTimers = false)
    {
        AsyncServiceScope spotScope = default;
        var scopeCreated = false;
        ZLinkSpotActivation? activation = null;
        try
        {
            spotScope = services.CreateAsyncScope();
            scopeCreated = true;
            activation = new ZLinkSpotActivation(
                runtime,
                spotScope,
                nativeSpot,
                spotId,
                node.RoutingId,
                registration.SpotNodeName,
                spotChannelName,
                frameworkRegistration.DefaultRequestTimeout,
                registration.Router?.SocketConfig.SendTimeout
                ?? frameworkRegistration.DefaultSocketSendTimeout,
                restoreLogicalTimers: restoreLogicalTimers,
                completionAdmission: completionAdmission,
                timerScheduler: timerScheduler);
            var spot = (IZLinkInstanceSpot)ActivatorUtilities.CreateInstance(
                spotScope.ServiceProvider,
                spotType,
                activation);
            activation.AttachInstanceSpot(spot);
            foreach (var handler in frameworkRegistration.ScannedHandlerCatalog.SpotHandlers)
                await activation.ApplyScannedHandlerAsync(handler, cancellationToken)
                    .ConfigureAwait(false);
            spot.Configure();
            await activation.BindDescriptorsAsync(cancellationToken).ConfigureAwait(false);
            await activation.InitializeInstanceAsync(cancellationToken).ConfigureAwait(false);
            return activation;
        }
        catch (Exception initializationFailure)
        {
            var failures = new ZLinkFailureCollector(initializationFailure);
            if (activation is null)
            {
                if (scopeCreated)
                    await failures.CaptureAsync(spotScope.DisposeAsync).ConfigureAwait(false);
                await failures.CaptureAsync(nativeSpot.DisposeAsync).ConfigureAwait(false);
            }
            else
            {
                await failures.CaptureAsync(activation.DisposeAsync).ConfigureAwait(false);
            }

            failures.ThrowIfAny();
            throw new InvalidOperationException(
                "Unreachable after Instance Spot startup cleanup failure propagation.");
        }
    }
}

internal readonly record struct ZLinkSpotActivationCreateResult(
    ZLinkSpotActivation Activation,
    ZLinkSpotCreateResponse Response);
