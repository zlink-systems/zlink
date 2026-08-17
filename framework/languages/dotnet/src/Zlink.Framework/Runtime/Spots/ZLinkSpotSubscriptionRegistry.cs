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

            foreach (var existing in handlers)
            {
                if (!string.Equals(
                        existing.MessageName,
                        descriptor.MessageName,
                        StringComparison.Ordinal))
                    continue;
                throw new ZLinkConfigurationException(
                    $"SPOT subscription handler for channel '{subscription.ChannelName}', topic '{subscription.Topic}' and packet '{descriptor.MessageName}' is already registered.");
            }
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

            using var applicationAdmission =
                message.ApplicationJobAdmission is { } admission
                    ? ZLinkApplicationJobQueueInvocation.Enter(admission)
                    : null;
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
        if (message.Parts.Count == 0)
            return;

        ZLinkEnvelopeHeader header;
        try
        {
            header = ZLinkEnvelopeCodec.DecodeHeader(
                message.Parts,
                dispatchErrors.Flow.CaptureEnabled);
            ZLinkEnvelopeCodec.ValidateDispatchHeader(header);
        }
        catch (ZLinkEnvelopeProtocolException)
        {
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
        if (descriptors is null)
            return;

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
        }
    }
}
