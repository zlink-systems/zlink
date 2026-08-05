namespace Zlink.Framework.Runtime.Actors;

internal static class ZLinkSessionBindingReplacement
{
    internal static ZLinkActorPreviousBindingFence? CreateFence(
        ZLinkRemoteSessionPreviousBinding? previous)
    {
        if (previous is null) return null;
        return new ZLinkActorPreviousBindingFence(
            RoutingId.From(previous.TargetNodeRid),
            RoutingId.From(previous.SessionNodeRid),
            RoutingId.From(previous.SessionRid),
            previous.BindingToken,
            previous.BindingGeneration,
            previous.ObjectGeneration,
            previous.MeshName,
            previous.TargetNodeGeneration,
            previous.AuthorityOwnerGeneration,
            previous.OwnerLeaseGeneration,
            previous.SessionOwnerNodeGeneration,
            previous.AcceptedHighWater);
    }

    internal static async ValueTask CompletePreviousAsync(
        string actorId,
        RoutingId targetNodeRid,
        ZLinkRemoteSessionPreviousBinding? previous,
        Func<ZLinkRemoteSessionUnbindRequest, CancellationToken,
            ValueTask<ZLinkRemoteSessionUnbindResponse>> sendTombstoneAsync,
        CancellationToken cancellationToken)
    {
        if (previous is null
            || RoutingId.From(previous.TargetNodeRid) == targetNodeRid)
            return;

        var response = await sendTombstoneAsync(
                new ZLinkRemoteSessionUnbindRequest(
                    actorId,
                    previous.TargetNodeRid,
                    previous.SessionNodeRid,
                    previous.SessionRid,
                    previous.BindingToken,
                    previous.BindingGeneration,
                    previous.ObjectGeneration,
                    previous.MeshName,
                    previous.TargetNodeGeneration,
                    previous.AuthorityOwnerGeneration,
                    previous.OwnerLeaseGeneration,
                    previous.SessionOwnerNodeGeneration,
                    previous.AcceptedHighWater),
                cancellationToken)
            .ConfigureAwait(false);
        if (!response.Acknowledged)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actorId}' previous session binding was not invalidated.",
                ZLinkRetryAdvice.RetryAfterBackoff);
    }
}
