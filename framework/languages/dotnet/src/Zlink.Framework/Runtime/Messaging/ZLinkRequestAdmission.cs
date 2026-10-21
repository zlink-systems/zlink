namespace Zlink.Framework.Runtime.Messaging;

// This source only observes acceptance by the existing transport/mailbox owner.
// It never grants capacity or determines whether an operation may be submitted.
internal sealed class ZLinkRequestAdmission
{
    private readonly TaskCompletionSource _completion = new(
        TaskCreationOptions.RunContinuationsAsynchronously
    );

    internal void Complete() => _completion.TrySetResult();

    internal bool IsAdmitted => _completion.Task.IsCompletedSuccessfully;

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
                new ValueTask(admission._completion.Task),
                ObserveAsync(ValueTask.FromException<TReply>(error), admission)
            );
        }
        // Once transport/mailbox has completed admission, the existing async
        // execution owns the reply without a further admission observer.
        return new ZLinkRequest<TReply>(
            new ValueTask(admission._completion.Task),
            admission.IsAdmitted ? reply : ObserveAsync(reply, admission)
        );

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
                admission._completion.TrySetException(error);
                throw;
            }
        }
    }
}
