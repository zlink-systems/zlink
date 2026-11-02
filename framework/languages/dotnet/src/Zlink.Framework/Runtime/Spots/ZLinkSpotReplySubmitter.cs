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
            {
                var result = received.Reply(replyParts);
                if (result != SubmitResult.Ok)
                    throw new ZlinkSubmitException((ZlinkSubmitException.ErrorCode)(int)result);
            }
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
    }

    public static ValueTask SubmitDirectAsync(
        ZLinkBackendRouteReceived received,
        IReadOnlyList<Message> replyParts
    )
    {
        try
        {
            SubmitAndDispose(received, replyParts);
            return ValueTask.CompletedTask;
        }
        catch (Exception error)
        {
            return ValueTask.FromException(error);
        }
    }
}
