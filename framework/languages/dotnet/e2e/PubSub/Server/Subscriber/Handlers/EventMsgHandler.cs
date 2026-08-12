using PubSub.Server.Subscriber.Configuration;
using PubSub.Shared;
using Zlink.Framework.Contracts.Handlers;

namespace PubSub.Server.Subscriber.Handlers;

internal sealed class EventMsgHandler(EvidenceStore evidence, HandlerDelayOptions delayOptions)
    : IZLinkFanoutHandler<PublishedValueEvent>
{
    public async ValueTask HandleAsync(
        PublishedValueEvent message,
        CancellationToken cancellationToken)
    {
        var topic = message.Value == "ignored"
            ? PubSubNames.OtherTopic
            : PubSubNames.MainTopic;
        cancellationToken.ThrowIfCancellationRequested();
        if (delayOptions.DelayMs > 0 && message.Value.StartsWith("slow-", StringComparison.Ordinal))
        {
            evidence.Add(
                $"delay-start|rid={evidence.Rid}|run={message.RunId}|topic={topic}"
                + $"|seq={message.Sequence}|value={message.Value}");
            await Task.Delay(delayOptions.DelayMs, cancellationToken);
        }

        if (topic == PubSubNames.MainTopic)
            evidence.Add(
                $"event|rid={evidence.Rid}|run={message.RunId}|topic={topic}"
                + $"|seq={message.Sequence}|value={message.Value}|packet={nameof(PublishedValueEvent)}");
        else
            evidence.Add(
                $"ignored|rid={evidence.Rid}|run={message.RunId}|topic={topic}"
                + $"|seq={message.Sequence}|value={message.Value}|packet={nameof(PublishedValueEvent)}");
    }
}
