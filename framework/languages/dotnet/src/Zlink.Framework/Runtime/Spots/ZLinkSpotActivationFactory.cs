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
    ZLinkTimerScheduler timerScheduler
)
{
    public async ValueTask<ZLinkSpotActivationCreateResult> CreateAsync(
        Type spotType,
        IZLinkBackendSpot nativeSpot,
        string spotId,
        ZLinkMessage request,
        CancellationToken cancellationToken
    ) =>
        await CreateUserSpotAsync(
                spotType,
                nativeSpot,
                spotId,
                request,
                invokeCreate: true,
                cancellationToken
            )
            .ConfigureAwait(false);

    internal async ValueTask<ZLinkSpotActivation> CreateForRelocationAsync(
        Type spotType,
        IZLinkBackendSpot nativeSpot,
        string spotId,
        CancellationToken cancellationToken,
        IReadOnlyList<ZLinkRelocationLogicalTimer>? logicalTimers = null
    ) =>
        (
            await CreateUserSpotAsync(
                    spotType,
                    nativeSpot,
                    spotId,
                    ZLinkMessage.Empty,
                    invokeCreate: false,
                    cancellationToken,
                    logicalTimers
                )
                .ConfigureAwait(false)
        ).Activation;

    private async ValueTask<ZLinkSpotActivationCreateResult> CreateUserSpotAsync(
        Type spotType,
        IZLinkBackendSpot nativeSpot,
        string spotId,
        ZLinkMessage request,
        bool invokeCreate,
        CancellationToken cancellationToken,
        IReadOnlyList<ZLinkRelocationLogicalTimer>? logicalTimers = null
    )
    {
        AsyncServiceScope spotScope = default;
        var scopeCreated = false;
        ZLinkUserSpotActivation? activation = null;
        try
        {
            spotScope = services.CreateAsyncScope();
            scopeCreated = true;
            activation = new ZLinkUserSpotActivation(
                runtime,
                spotScope,
                nativeSpot,
                spotId,
                node.RoutingId,
                registration.SpotNodeName,
                spotChannelName,
                frameworkRegistration.DefaultRequestTimeout,
                registration.UserSpotFactoryOptions.TryGetValue(spotType, out var userSpotOptions)
                    ? userSpotOptions.ExecutionMode
                    : ZLinkUserSpotExecutionMode.SpotWide,
                userSpotOptions?.RelocationCoordinationMode
                    ?? ZLinkSpotRelocationCoordinationMode.FrameworkManaged,
                restoreLogicalTimers: !invokeCreate,
                timerScheduler: timerScheduler
            );

            var spot = (IZLinkSpot)
                ActivatorUtilities.CreateInstance(spotScope.ServiceProvider, spotType, activation);

            activation.AttachSpot(spot);

            var response = ZLinkSpotCreateResponse.Accept();
            async ValueTask Initialize(CancellationToken ct)
            {
                await activation
                    .ConfigureAsync(
                        frameworkRegistration.ScannedHandlerCatalog.SpotHandlers,
                        spot.Configure,
                        logicalTimers,
                        ct
                    )
                    .ConfigureAwait(false);
                await activation.BindDescriptorsAsync(ct).ConfigureAwait(false);
                if (!invokeCreate)
                    activation.AttachNativeDispatch();
                response = invokeCreate
                    ? await activation.InitializeAsync(request, ct).ConfigureAwait(false)
                    : ZLinkSpotCreateResponse.Accept();
            }
            if (invokeCreate)
                await activation
                    ._serial.PrepareActivationAsync(Initialize, cancellationToken)
                    .ConfigureAwait(false);
            else
            {
                await activation._serial.BeginTargetPreparationAsync().ConfigureAwait(false);
                await activation
                    ._serial.ExecuteTargetPreparationAsync(
                        (_, ct) => Initialize(ct),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
            }
            return new ZLinkSpotActivationCreateResult(activation, response);
        }
        catch (Exception initializationFailure)
        {
            initializationFailure = ConfigurationFailure(initializationFailure);
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
                "Unreachable after startup cleanup failure propagation."
            );
        }
    }

    public async ValueTask<ZLinkSpotActivation> CreateInstanceAsync(
        Type spotType,
        IZLinkBackendSpot nativeSpot,
        string spotId,
        CancellationToken cancellationToken,
        bool restoreLogicalTimers = false,
        IReadOnlyList<ZLinkRelocationLogicalTimer>? logicalTimers = null,
        bool relocationTarget = false
    )
    {
        AsyncServiceScope spotScope = default;
        var scopeCreated = false;
        ZLinkInstanceSpotActivation? activation = null;
        try
        {
            spotScope = services.CreateAsyncScope();
            scopeCreated = true;
            activation = new ZLinkInstanceSpotActivation(
                runtime,
                spotScope,
                nativeSpot,
                spotId,
                node.RoutingId,
                registration.SpotNodeName,
                spotChannelName,
                frameworkRegistration.DefaultRequestTimeout,
                restoreLogicalTimers: restoreLogicalTimers,
                timerScheduler: timerScheduler
            );
            var spot = (IZLinkInstanceSpot)
                ActivatorUtilities.CreateInstance(spotScope.ServiceProvider, spotType, activation);
            activation.AttachInstanceSpot(spot);
            async ValueTask Initialize(CancellationToken ct)
            {
                await activation
                    .ConfigureAsync(
                        frameworkRegistration.ScannedHandlerCatalog.SpotHandlers,
                        spot.Configure,
                        logicalTimers,
                        ct
                    )
                    .ConfigureAwait(false);
                await activation.BindDescriptorsAsync(ct).ConfigureAwait(false);
                await activation.InitializeAsync(ct).ConfigureAwait(false);
            }
            if (relocationTarget)
            {
                await activation._serial.BeginTargetPreparationAsync().ConfigureAwait(false);
                // The staging seal owns the target's lifecycle; initialization
                // executes under that owner instead of a second lane item.
                await activation
                    ._serial.ExecuteTargetPreparationAsync(
                        (_, ct) => Initialize(ct),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
            }
            else
                await activation
                    ._serial.PrepareActivationAsync(Initialize, cancellationToken)
                    .ConfigureAwait(false);
            return activation;
        }
        catch (Exception initializationFailure)
        {
            initializationFailure = ConfigurationFailure(initializationFailure);
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
                "Unreachable after Instance Spot startup cleanup failure propagation."
            );
        }
    }

    internal static Exception ConfigurationFailure(Exception failure) =>
        failure is ZLinkConfigurationException configuration
            ? new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                configuration.Message,
                innerException: configuration
            )
            : failure;
}

internal readonly record struct ZLinkSpotActivationCreateResult(
    ZLinkSpotActivation Activation,
    ZLinkSpotCreateResponse Response
);
