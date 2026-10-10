namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkRequestSubmissionOutcome
{
    internal static Task<IReadOnlyList<Message>> SubmitAndAwaitReplyAsync(
        RequestSubmitOperation operation,
        TimeSpan timeout,
        CancellationToken cancellationToken = default,
        Service.ZLinkServiceLiveness? receiveAdmission = null
    )
    {
        var started = System.Diagnostics.Stopwatch.GetTimestamp();
        // Binding owns the reply timeout and caller cancellation. Only an
        // initial admission wait needs the Framework's end-to-end budget.
        var submission = operation.Async(cancellationToken);
        if (!ZLinkBindingSubmissionOutcome.RequiresAdmission(submission.Result))
            return AwaitReplyAsync(submission.Result, submission.Admitted, submission.Reply);

        var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        try
        {
            return AwaitWithDeadlineAsync(
                AwaitReplyAsync(submission.Result, submission.Admitted, submission.Reply),
                deadline,
                cancellationToken,
                CancelAtDeadlineAsync(deadline, timeout, started),
                receiveAdmission
            );
        }
        catch
        {
            deadline.Dispose();
            throw;
        }
    }

    // .NET timers truncate fractional milliseconds. A remaining request
    // budget must reach its last millisecond before the timer expires.
    internal static TimeSpan TimerDueTime(TimeSpan timeout) =>
        TimeSpan.FromMilliseconds(Math.Ceiling(timeout.TotalMilliseconds));

    private static async Task<IReadOnlyList<Message>> AwaitWithDeadlineAsync(
        Task<IReadOnlyList<Message>> completion,
        CancellationTokenSource deadline,
        CancellationToken cancellationToken,
        Task deadlineTimer,
        Service.ZLinkServiceLiveness? receiveAdmission
    )
    {
        using (deadline)
        {
            // The existing request completion owner releases late replies and
            // prevents cancellation callbacks from re-entering a state lane.
            using var callerCompletion = new ZLinkRequestCompletion<IReadOnlyList<Message>>(
                deadline.Token,
                discardResult: ZLinkMessageParts.DisposeAll,
                receiveAdmission: receiveAdmission
            );
            _ = ObserveCompletionAsync(completion, callerCompletion);
            try
            {
                return await callerCompletion.Task.ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
            {
                throw new ZlinkRequestException(ZlinkRequestException.ErrorCode.TimedOut);
            }
            finally
            {
                await deadline.CancelAsync().ConfigureAwait(false);
                await deadlineTimer.ConfigureAwait(false);
            }
        }
    }

    private static async Task CancelAtDeadlineAsync(
        CancellationTokenSource deadline,
        TimeSpan timeout,
        long started
    )
    {
        var token = deadline.Token;
        try
        {
            while (true)
            {
                token.ThrowIfCancellationRequested();
                var remaining = timeout - System.Diagnostics.Stopwatch.GetElapsedTime(started);
                if (remaining <= TimeSpan.Zero)
                {
                    await deadline.CancelAsync().ConfigureAwait(false);
                    return;
                }
                await Task.Delay(TimerDueTime(remaining), token).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested)
        {
            // The request's caller or completion ended the existing deadline wait.
        }
    }

    private static async Task ObserveCompletionAsync(
        Task<IReadOnlyList<Message>> completion,
        ZLinkRequestCompletion<IReadOnlyList<Message>> callerCompletion
    )
    {
        try
        {
            callerCompletion.Complete(await completion.ConfigureAwait(false));
        }
        catch (Exception error)
        {
            callerCompletion.Fail(error);
        }
    }

    internal static Task<IReadOnlyList<Message>> AwaitReplyAsync(
        SubmitResult result,
        Task admitted,
        Task<IReadOnlyList<Message>> reply
    ) =>
        ZLinkBindingSubmissionOutcome.RequiresAdmission(result)
            ? AwaitAdmissionAndReplyAsync(admitted, reply)
            : reply;

    private static async Task<IReadOnlyList<Message>> AwaitAdmissionAndReplyAsync(
        Task admitted,
        Task<IReadOnlyList<Message>> reply
    )
    {
        await Task.WhenAll(admitted, reply).ConfigureAwait(false);
        return await reply.ConfigureAwait(false);
    }
}
