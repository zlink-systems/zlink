using Microsoft.Extensions.Logging;
using System.Diagnostics;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotSubscriptionRegistry
{
    private readonly Dictionary<ZLinkSpotSubscriptionKey, List<ZLinkSpotSubscriptionDescriptor>>
        _descriptorsByTarget = new();

    private readonly List<ZLinkSpotSubscriptionRegistration> _registrations = [];

    public void Add(string channelName, string topic, Type handlerType)
    {
        if (string.IsNullOrWhiteSpace(channelName))
            throw new ZLinkConfigurationException("SPOT subscription channel name must not be empty.");
        if (string.IsNullOrWhiteSpace(topic))
            throw new ZLinkConfigurationException("SPOT subscription topic must not be empty.");

        _registrations.Add(new ZLinkSpotSubscriptionRegistration(channelName, topic, handlerType));
    }

    public void Add(
        string channelName,
        string topic,
        Type spotType,
        System.Reflection.MethodInfo method)
    {
        if (string.IsNullOrWhiteSpace(channelName))
            throw new ZLinkConfigurationException("SPOT subscription channel name must not be empty.");
        if (string.IsNullOrWhiteSpace(topic))
            throw new ZLinkConfigurationException("SPOT subscription topic must not be empty.");
        _registrations.Add(new ZLinkSpotSubscriptionRegistration(channelName, topic, spotType, method));
    }

    public async ValueTask BindAsync(
        object spot,
        IZLinkBackendSpot nativeSpot,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        foreach (var (target, handlers) in BuildDescriptors(spot))
        {
            _descriptorsByTarget.Add(target, handlers);
            await SetSubscriptionAsync(
                    nativeSpot, target.ChannelName, target.Topic, timeout, cancellationToken)
                .ConfigureAwait(false);
        }
    }

    public void Bind(object spot, IZLinkBackendSpot nativeSpot)
    {
        foreach (var (target, handlers) in BuildDescriptors(spot))
        {
            _descriptorsByTarget.Add(target, handlers);
            nativeSpot.SetSubscription(target.ChannelName, target.Topic);
        }
    }

    private static async ValueTask SetSubscriptionAsync(
        IZLinkBackendSpot nativeSpot,
        string channelName,
        string topic,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var started = Stopwatch.GetTimestamp();
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                nativeSpot.SetSubscription(channelName, topic);
                return;
            }
            catch (ZlinkConfigException error)
                when (error.Result == ZlinkConfigException.ErrorCode.InternalError
                      && Stopwatch.GetElapsedTime(started) < timeout)
            {
                var remaining = timeout - Stopwatch.GetElapsedTime(started);
                await Task.Delay(
                        remaining < TimeSpan.FromMilliseconds(25)
                            ? remaining
                            : TimeSpan.FromMilliseconds(25),
                        cancellationToken)
                    .ConfigureAwait(false);
            }
        }
    }

    public void Validate(object spot)
    {
        _ = BuildDescriptors(spot);
    }

    private Dictionary<ZLinkSpotSubscriptionKey, List<ZLinkSpotSubscriptionDescriptor>> BuildDescriptors(
        object spot)
    {
        var descriptorsByTarget =
            new Dictionary<ZLinkSpotSubscriptionKey, List<ZLinkSpotSubscriptionDescriptor>>();
        foreach (var subscription in _registrations)
        {
            var descriptor = subscription.Method is { } method
                ? ZLinkSpotDescriptorFactory.CreateAttributedSubscriptionDescriptor(
                    subscription.ChannelName, subscription.Topic, subscription.HandlerType, method)
                : ZLinkSpotDescriptorFactory.CreateSubscriptionDescriptor(
                    subscription.ChannelName, subscription.Topic, subscription.HandlerType, spot.GetType());

            var target = new ZLinkSpotSubscriptionKey(
                subscription.ChannelName,
                subscription.Topic);

            if (!descriptorsByTarget.TryGetValue(target, out var handlers))
            {
                handlers = [];
                descriptorsByTarget.Add(target, handlers);
            }

            if (handlers.Any(existing => string.Equals(
                    existing.MessageName,
                    descriptor.MessageName,
                    StringComparison.Ordinal)))
                throw new ZLinkConfigurationException(
                    $"SPOT subscription handler for channel '{subscription.ChannelName}', topic '{subscription.Topic}' and packet '{descriptor.MessageName}' is already registered.");
            handlers.Add(descriptor);
        }

        return descriptorsByTarget;
    }

    public async ValueTask DrainAsync(
        IZLinkBackendSpot nativeSpot,
        ZLinkCodecRegistryBuilder? codecs,
        ZLinkDispatchErrorReporter dispatchErrors,
        ILogger logger,
        Func<ZLinkSpotSubscriptionDescriptor, object?, ZLinkPublishMessageContext, CancellationToken, ValueTask>
            dispatchAsync,
        CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            ZLinkBackendSubscribeMessage? message;
            try
            {
                message = nativeSpot.Subscribe(RecvFlags.DontWait);
            }
            catch (ZlinkRecvException ex)
                when (ex.Result is ZlinkRecvException.ErrorCode.NoData
                          or ZlinkRecvException.ErrorCode.Busy)
            {
                return;
            }

            if (message is null) return;

            using (message)
                await DispatchMessageAsync(
                        message, codecs, dispatchErrors, logger, dispatchAsync, cancellationToken)
                    .ConfigureAwait(false);
        }
    }

    private async ValueTask DispatchMessageAsync(
        ZLinkBackendSubscribeMessage message,
        ZLinkCodecRegistryBuilder? codecs,
        ZLinkDispatchErrorReporter dispatchErrors,
        ILogger logger,
        Func<ZLinkSpotSubscriptionDescriptor, object?, ZLinkPublishMessageContext, CancellationToken, ValueTask>
            dispatchAsync,
        CancellationToken cancellationToken)
    {
        message.StartDispatch();
        if (message.Parts.Count == 0)
        {
            using var invalidFlow = ZLinkFlowContext.Enter(
                null,
                null,
                dispatchErrors.Flow.CaptureEnabled,
                ZLinkFlowOrigin.Inbound);
            CreateScope("<unknown>", message.Topic)
                .Dropped(
                    logger,
                    dispatchErrors,
                    LogLevel.Warning,
                    ZLinkDispatchErrorReason.InvalidFrame,
                    "invalid-frame");
            return;
        }

        ZLinkEnvelopeHeader header;
        try
        {
            header = ZLinkEnvelopeCodec.DecodeHeader(message.Parts);
            ZLinkEnvelopeCodec.ValidateDispatchHeader(header);
        }
        catch (ZLinkEnvelopeProtocolException protocolError)
        {
            var validFlow = ZLinkEnvelopeCodec.ValidFlow(protocolError.Header);
            using var invalidFlow = ZLinkFlowContext.Enter(
                validFlow.FlowId,
                validFlow.FlowOrigin,
                dispatchErrors.Flow.CaptureEnabled,
                ZLinkFlowOrigin.Inbound);
            CreateScope(
                    protocolError.Header.MessageName,
                    message.Topic,
                    protocolError.Header.ContentType,
                    protocolError.Header.CorrelationId,
                    protocolError.Header.Source)
                .Dropped(
                    logger,
                    dispatchErrors,
                    LogLevel.Warning,
                    ZLinkDispatchErrorReason.InvalidFrame,
                    "invalid-frame");
            return;
        }
        using var currentFlow = ZLinkFlowContext.Enter(
            header.FlowId,
            header.FlowOrigin,
            dispatchErrors.Flow.CaptureEnabled,
            ZLinkFlowOrigin.Inbound);

        _descriptorsByTarget.TryGetValue(
            new ZLinkSpotSubscriptionKey(message.ChannelName, message.Topic),
            out var descriptors);
        var scope = CreateScope(
            header.MessageName,
            message.Topic,
            header.ContentType,
            header.CorrelationId,
            header.Source);

        scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Received);

        if (descriptors is null)
        {
            scope.Dropped(logger, dispatchErrors, LogLevel.Debug);
            return;
        }

        var dispatched = false;
        var context = new ZLinkPublishMessageContext(
            meshName: null,
            message.ChannelName,
            header.MessageName!,
            header.ContentType,
            metadata: null,
            header.CorrelationId,
            message.Topic,
            header.Source);
        foreach (var descriptor in descriptors)
        {
            if (!string.Equals(descriptor.MessageName, header.MessageName, StringComparison.Ordinal)) continue;

            var body = ZLinkEnvelopeCodec.DecodeBody(message.Parts, descriptor.MessageType, codecs);
            await dispatchAsync(descriptor, body, context, cancellationToken).ConfigureAwait(false);
            dispatched = true;
        }

        if (dispatched)
            scope.Trace(dispatchErrors, ZLinkMessageFlowOutcome.Dispatched);

        if (!dispatched)
        {
            scope.Dropped(logger, dispatchErrors, LogLevel.Debug);
        }
    }

    private static ZLinkDispatchFlowScope CreateScope(
        string? packetName,
        string topic,
        string? contentType = null,
        string? correlationId = null,
        string? sourceRid = null)
    {
        return new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.SpotSubscription,
            "SpotSubscription",
            ZLinkDispatchMessageKind.Publish,
            "Publish",
            packetName ?? "<unknown>",
            topic: topic,
            contentType: contentType,
            correlationId: correlationId,
            sourceRid: sourceRid);
    }
}
