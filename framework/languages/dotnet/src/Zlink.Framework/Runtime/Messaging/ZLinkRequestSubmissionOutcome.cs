namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkRequestSubmissionOutcome
{
    internal static Task<IReadOnlyList<Message>> SubmitAndAwaitReplyAsync(
        RequestSubmitOperation operation,
        TimeSpan timeout,
        CancellationToken cancellationToken = default,
        Service.ZLinkServiceLiveness? receiveAdmission = null,
        ZLinkRequestAdmission? requestAdmission = null,
        ZLinkRequestDeadlineIndex? deadlines = null
    )
    {
        if (deadlines is not null)
            return SubmitIndexedAsync(
                operation,
                timeout,
                cancellationToken,
                receiveAdmission,
                requestAdmission,
                deadlines
            );
        return SubmitAndAwaitReplyAsync(
            operation,
            timeout,
            out _,
            cancellationToken,
            receiveAdmission,
            requestAdmission
        );
    }

    private static Task<IReadOnlyList<Message>> SubmitIndexedAsync(
        RequestSubmitOperation operation,
        TimeSpan timeout,
        CancellationToken token,
        Service.ZLinkServiceLiveness? admission,
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
                return SubmitAndAwaitReplyAsync(
                    operation,
                    remaining,
                    out _,
                    token,
                    admission,
                    requestAdmission
                );
            }
        );

    internal static Task<IReadOnlyList<Message>> SubmitAndAwaitReplyAsync(
        RequestSubmitOperation operation,
        TimeSpan timeout,
        out Task nativeAdmission,
        CancellationToken cancellationToken = default,
        Service.ZLinkServiceLiveness? receiveAdmission = null,
        ZLinkRequestAdmission? requestAdmission = null
    )
    {
        var submission = operation.Async(cancellationToken);
        if (!ZLinkBindingSubmissionOutcome.RequiresAdmission(submission.Result, requestAdmission))
        {
            nativeAdmission = Task.CompletedTask;
            return submission.Reply;
        }
        var admitted = requestAdmission is null
            ? submission.Admitted
            : ObserveAdmissionAsync(submission.Admitted, requestAdmission);
        nativeAdmission = admitted;
        return AwaitAdmissionAndReplyAsync(admitted, submission.Reply);
    }

    private static async Task ObserveAdmissionAsync(
        Task admitted,
        ZLinkRequestAdmission? requestAdmission
    )
    {
        await admitted.ConfigureAwait(false);
        requestAdmission?.Complete();
    }

    private static async Task<IReadOnlyList<Message>> AwaitAdmissionAndReplyAsync(
        Task admitted,
        Task<IReadOnlyList<Message>> reply
    )
    {
        try
        {
            await admitted.ConfigureAwait(false);
        }
        catch
        {
            _ = DiscardReplyAsync(reply);
            throw;
        }
        return await reply.ConfigureAwait(false);
    }

    private static async Task DiscardReplyAsync(Task<IReadOnlyList<Message>> reply)
    {
        try
        {
            ZLinkMessageParts.DisposeAll(await reply.ConfigureAwait(false));
        }
        catch (Exception error)
        {
            ZLinkFrameworkDebugLog.TaskFailure("unadmitted-late-reply", error);
        }
    }
}
