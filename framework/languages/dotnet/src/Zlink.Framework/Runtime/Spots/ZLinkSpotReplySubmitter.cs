using Zlink.Framework.Runtime.Dispatch;
namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotReplySubmitter
{
    private static ulong Measure(IReadOnlyList<Message> replyParts)
    {
        ulong replyBytes = 0;
        foreach (var part in replyParts)
            replyBytes = checked(replyBytes + (ulong)Math.Max(part.Size, 1));
        return replyBytes;
    }

    public static void SubmitAndDispose(
        ZLinkBackendRouteReceived received, IReadOnlyList<Message> replyParts)
    {
        try
        {
            if (received.CanReply)
            {
                var result = received.Reply(replyParts);
                if (result != SubmitResult.Ok)
                    throw new ZlinkSubmitException(
                        (ZlinkSubmitException.ErrorCode)(int)result);
            }
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
    }

    public static async ValueTask SubmitDirectAsync(
        ZLinkBackendRouteReceived received,
        IReadOnlyList<Message> replyParts,
        ZLinkCompletionAdmissionOwner.ResponderLease? completionLease,
        CancellationToken cancellationToken)
    {
        //  Infrastructure requests - session route commit, seal, abort and
        //  unseal - carry no responder lease on purpose: their replies must not
        //  queue behind application completion admission, or the relocation
        //  control plane would stall whenever application traffic filled it.
        //  They still need an answer, so send without reserving.
        if (completionLease is not null)
            try
            {
                await completionLease.ReserveReplyAsync(
                        Measure(replyParts), cancellationToken)
                    .ConfigureAwait(false);
            }
            catch
            {
                ZLinkMessageParts.DisposeAll(replyParts);
                throw;
            }

        SubmitAndDispose(received, replyParts);
        completionLease?.TransferToCore();
    }

    public static async ValueTask SubmitAsync(
        ZLinkAsyncSubmitter submitter,
        ZLinkBackendRouteReceived received,
        IReadOnlyList<Message> replyParts,
        ZLinkCompletionAdmissionOwner.ResponderLease? completionLease,
        CancellationToken cancellationToken)
    {
        //  Infrastructure requests - session route commit, seal, abort and
        //  unseal - carry no responder lease on purpose: their replies must not
        //  queue behind application completion admission, or the relocation
        //  control plane would stall whenever application traffic filled it.
        //  They still need an answer, so send without reserving.
        if (completionLease is not null)
            try
            {
                await completionLease.ReserveReplyAsync(
                        Measure(replyParts), cancellationToken)
                    .ConfigureAwait(false);
            }
            catch
            {
                ZLinkMessageParts.DisposeAll(replyParts);
                throw;
            }

        var result = await submitter.SubmitAsync(
                replyParts,
                pending => ZLinkSubmitFailureMapper.AcceptOrThrow(
                    received.Reply(pending, SendFlags.DontWait),
                    nameof(SubmitAsync)),
                cancellationToken)
            .ConfigureAwait(false);
        if (result.Status != ZLinkOneWaySubmitStatus.Submitted)
            throw new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.Terminated);
        completionLease?.TransferToCore();
    }
}
