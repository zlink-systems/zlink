namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkBoundActorRelaySender(TimeSpan timeout)
{
    public async ValueTask SendAsync(
        ZLinkManagedStream managedStream,
        ZLinkSessionActor actorRef,
        ZlinkStreamHeader header,
        Message payload,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var headerBytes = ZLinkStreamProtocolDefaults.EncodeHeader(header).ToArray();
        var bodyBytes = payload.ToArray();

        await ZLinkRetryingSubmitter.Async(
                () =>
                {
                    using var headerPart = Message.From(headerBytes);
                    using var bodyPart = Message.From(bodyBytes);
                    return managedStream.SendBoundActor(
                        actorRef.ActorId,
                        new[] { headerPart, bodyPart });
                },
                timeout,
                "Actor session relay failed because the session relay route was not ready before timeout.",
                cancellationToken)
            .ConfigureAwait(false);
    }
}