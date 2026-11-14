namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkRequestSubmissionOutcome
{
    internal static Task<IReadOnlyList<Message>> SubmitAndAwaitReplyAsync(
        RequestSubmitOperation operation,
        TimeSpan timeout,
        CancellationToken cancellationToken = default,
        ZLinkRequestAdmission? requestAdmission = null,
        ZLinkRequestDeadlineIndex? deadlines = null
    )
    {
        if (deadlines is not null)
            return SubmitIndexedAsync(
                operation,
                timeout,
                cancellationToken,
                requestAdmission,
                deadlines
            );
        return SubmitAndAwaitReplyAsync(operation, out _, cancellationToken, requestAdmission);
    }

    private static Task<IReadOnlyList<Message>> SubmitIndexedAsync(
        RequestSubmitOperation operation,
        TimeSpan timeout,
        CancellationToken token,
        ZLinkRequestAdmission? requestAdmission,
        ZLinkRequestDeadlineIndex deadlines
    ) =>
        deadlines.RunAsync(
            timeout,
            token,
            (entry, completion) =>
            {
                var remaining = entry.Remaining;
                if (!entry.IsPending || remaining <= TimeSpan.Zero)
                    return completion;
                return SubmitAndAwaitReplyAsync(operation, out _, token, requestAdmission);
            }
        );

    internal static Task<IReadOnlyList<Message>> SubmitAndAwaitReplyAsync(
        RequestSubmitOperation operation,
        out Task nativeAdmission,
        CancellationToken cancellationToken = default,
        ZLinkRequestAdmission? requestAdmission = null
    )
    {
        var submission = operation.Async(cancellationToken);
        if (!ZLinkBindingSubmissionOutcome.RequiresAdmission(submission.Result, requestAdmission))
        {
            nativeAdmission = Task.CompletedTask;
            return submission.Reply;
        }
        nativeAdmission = submission.Admitted;
        return requestAdmission is null
            ? submission.Reply
            : ObserveAdmissionAsync(submission, requestAdmission);
    }

    private static async Task<IReadOnlyList<Message>> ObserveAdmissionAsync(
        RequestSubmission submission,
        ZLinkRequestAdmission requestAdmission
    )
    {
        IReadOnlyList<Message> reply;
        try
        {
            await submission.Admitted.ConfigureAwait(false);
            requestAdmission.Complete();
        }
        finally
        {
            // Binding reports the same admission failure on Reply. Keep its physical
            // result under the existing request receiver, even after public completion.
            reply = await submission.Reply.ConfigureAwait(false);
        }
        return reply;
    }
}
