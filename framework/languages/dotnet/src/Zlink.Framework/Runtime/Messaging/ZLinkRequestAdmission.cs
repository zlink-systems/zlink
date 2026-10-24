namespace Zlink.Framework.Runtime.Messaging;

// This source only observes acceptance by the existing transport/mailbox owner.
// It never grants capacity or determines whether an operation may be submitted.
internal sealed class ZLinkRequestAdmission
{
    // The existing completion slot holds either the successful admission task or
    // the source needed when admission is still pending at observation time.
    private object? _completion;

    internal void Complete()
    {
        if (
            Interlocked.CompareExchange(ref _completion, Task.CompletedTask, null)
            is TaskCompletionSource pending
        )
            pending.TrySetResult();
    }

    internal bool IsAdmitted =>
        Volatile.Read(ref _completion) switch
        {
            Task completed => completed.IsCompletedSuccessfully,
            TaskCompletionSource pending => pending.Task.IsCompletedSuccessfully,
            _ => false,
        };

    private object ObserveCompletion()
    {
        var completion = Volatile.Read(ref _completion);
        if (completion is not null)
            return completion;
        var pending = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        return Interlocked.CompareExchange(ref _completion, pending, null) ?? pending;
    }

    private ValueTask Admitted =>
        ObserveCompletion() switch
        {
            Task completed => new ValueTask(completed),
            TaskCompletionSource pending => new ValueTask(pending.Task),
            _ => throw new InvalidOperationException("Unknown admission completion storage."),
        };

    internal static ZLinkRequest<TReply> Run<TReply>(
        Func<ZLinkRequestAdmission, ValueTask<TReply>> execute
    ) => Run(execute, static (execute, admission) => execute(admission));

    // The state form lets a call pass itself and its token without a closure
    // and delegate per request.
    internal static ZLinkRequest<TReply> Run<TState, TReply>(
        TState state,
        Func<TState, ZLinkRequestAdmission, ValueTask<TReply>> execute
    )
    {
        var admission = new ZLinkRequestAdmission();
        ValueTask<TReply> reply;
        try
        {
            reply = execute(state, admission);
        }
        catch (Exception error)
        {
            return new ZLinkRequest<TReply>(
                admission.Admitted,
                ObserveAsync(ValueTask.FromException<TReply>(error), admission)
            );
        }
        // Once transport/mailbox has completed admission, the existing async
        // execution owns the reply without a further admission observer.
        reply = admission.IsAdmitted ? reply : ObserveAsync(reply, admission);
        return new ZLinkRequest<TReply>(admission.Admitted, reply);

        static async ValueTask<TReply> ObserveAsync(
            ValueTask<TReply> completion,
            ZLinkRequestAdmission admission
        )
        {
            try
            {
                var reply = await completion.ConfigureAwait(false);
                // A successful reply proves admission even when the execution
                // path did not expose its earlier acceptance notification.
                admission.Complete();
                return reply;
            }
            catch (Exception error)
            {
                if (admission.ObserveCompletion() is TaskCompletionSource pending)
                    pending.TrySetException(error);
                throw;
            }
        }
    }
}
