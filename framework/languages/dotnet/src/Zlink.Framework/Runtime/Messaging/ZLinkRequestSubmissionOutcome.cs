namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkRequestSubmissionOutcome
{
    internal static Task<IReadOnlyList<Message>> SubmitAndAwaitReplyAsync(
        RequestSubmitOperation operation,
        CancellationToken cancellationToken = default
    )
    {
        // Keep Async outside an async method so immediate submit failures stay synchronous.
        var submission = operation.Async(cancellationToken);
        return AwaitReplyAsync(submission.Result, submission.Admitted, submission.Reply);
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
