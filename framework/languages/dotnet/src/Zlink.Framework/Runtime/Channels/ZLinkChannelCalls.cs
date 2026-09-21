using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkPublishCall(
    ZLinkFrameworkRuntime runtime,
    ZLinkFrameworkRegistration registration,
    string channelName,
    string topic,
    object? message
) : IZLinkFanoutPublishCall
{
    private readonly ZLinkOneWayCallGate _submission = new("Fanout publish");

    public ValueTask Async(CancellationToken cancellationToken = default)
    {
        _submission.Claim();
        using var operation = runtime.EnterOperation();
        using var flow = ZLinkFlowContext.EnterCurrentOrCreate(
            ZLinkFlowOrigin.Application,
            runtime.Flow.CaptureEnabled
        );
        cancellationToken.ThrowIfCancellationRequested();
        var (publisher, envelopedMsg) = Build();
        try
        {
            // Binding publish defaults to an immediate attempt; the Framework
            // contract explicitly selects Core's send-timeout admission path.
            publisher.Publish(topic).Messages(envelopedMsg).Flags(SendFlags.None).Submit();
        }
        catch (ZlinkSubmitException failure)
        {
            throw ZLinkRequestFailureMapper.CreateSubmitException(failure, "Fanout publish");
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(envelopedMsg);
        }

        return ValueTask.CompletedTask;
    }

    private (IPubSocket Publisher, IReadOnlyList<Message> Message) Build()
    {
        var bundle = runtime.GetPublisherBundle(channelName);
        var publisher = (IPubSocket)bundle.Socket;
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Publish,
            channelName,
            ZLinkMessageNameResolver.ResolveFromMessage(message),
            topic: topic,
            source: channelName,
            includeCorrelationId: false,
            includeDeadline: false
        );
        var envelopedMsg = ZLinkEnvelopeCodec.EncodeParts(
            header,
            message,
            message?.GetType(),
            registration.Codecs
        );
        return (publisher, envelopedMsg);
    }
}
