namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotReplySubmitter
{
    public static void SubmitAndDispose(
        ZLinkBackendRouteReceived received,
        IReadOnlyList<Message> replyParts
    )
    {
        try
        {
            if (received.CanReply)
                EnsureSubmitted(received.Reply(replyParts));
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
    }

    internal static void EnsureSubmitted(SubmitResult result)
    {
        if (result != SubmitResult.Ok)
            throw new ZlinkSubmitException((ZlinkSubmitException.ErrorCode)(int)result);
    }

    public static async ValueTask SubmitDirectAsync(
        ZLinkBackendRouteReceived received,
        IReadOnlyList<Message> replyParts,
        CancellationToken cancellationToken
    )
    {
        SubmitAndDispose(received, replyParts);
        await ValueTask.CompletedTask;
    }
}
