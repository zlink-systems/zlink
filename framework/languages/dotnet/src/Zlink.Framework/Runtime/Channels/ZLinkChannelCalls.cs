using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkPublishCall(
    ZLinkFrameworkRuntime runtime,
    ZLinkFrameworkRegistration registration,
    string channelName,
    string topic,
    object? message)
    : IZLinkFanoutPublishCall
{
    private readonly ZLinkOneWayCallGate _submission = new("Fanout publish");

    public async ValueTask Async(
        CancellationToken cancellationToken = default)
    {
        _submission.Claim();
        using var operation = runtime.EnterOperation();
        using var flow = ZLinkFlowContext.EnterCurrentOrCreate(
            ZLinkFlowOrigin.Application,
            runtime.Flow.CaptureEnabled);
        cancellationToken.ThrowIfCancellationRequested();
        var (bundle, publisher, envelopedMsg) = Build();
        var result = await (bundle.Submitter
                    ?? throw new InvalidOperationException(
                        "ZLink publish submitter is not initialized."))
                .SubmitAsync(
                    envelopedMsg,
                    pending => publisher.Publish(topic, pending, SendFlags.DontWait),
                    cancellationToken)
                .ConfigureAwait(false);
        ZLinkOneWaySubmitOutcome.EnsureAccepted(result, "Fanout publish");
    }

    private (ZLinkChannelRuntimeBundle Bundle, IZLinkBackendPublisherSocket Publisher,
        IReadOnlyList<Message> Message) Build()
    {
        var bundle = runtime.GetPublisherBundle(channelName);
        var publisher = (IZLinkBackendPublisherSocket)bundle.Socket;
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Publish,
            channelName,
            ZLinkMessageNameResolver.ResolveFromMessage(message),
            topic: topic,
            source: channelName,
            includeCorrelationId: false,
            includeDeadline: false);
        var envelopedMsg = ZLinkEnvelopeCodec.EncodeParts(
            header, message, message?.GetType(), registration.Codecs);
        return (bundle, publisher, envelopedMsg);
    }
}
