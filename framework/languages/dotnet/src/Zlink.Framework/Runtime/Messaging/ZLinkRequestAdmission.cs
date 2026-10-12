namespace Zlink.Framework.Runtime.Messaging;

// This source only observes acceptance by the existing transport/mailbox owner.
// It never grants capacity or determines whether an operation may be submitted.
internal sealed class ZLinkRequestAdmission
{
    private readonly TaskCompletionSource _completion = new(
        TaskCreationOptions.RunContinuationsAsynchronously
    );

    internal void Complete() => _completion.TrySetResult();

    internal static ZLinkRequest<TReply> Run<TReply>(
        Func<ZLinkRequestAdmission, ValueTask<TReply>> execute
    )
    {
        var admission = new ZLinkRequestAdmission();
        return new ZLinkRequest<TReply>(new ValueTask(admission._completion.Task), ObserveAsync());

        async ValueTask<TReply> ObserveAsync()
        {
            try
            {
                return await execute(admission).ConfigureAwait(false);
            }
            catch (Exception error)
            {
                admission._completion.TrySetException(error);
                throw;
            }
        }
    }
}
